/*******************************************************************************
* Copyright 2021-2022 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include <cassert>
#include <stdexcept>

#include "loop_sequencer.hpp"
#include "utils.hpp"

namespace dnnl {
namespace impl {
namespace gpu {
namespace jit {

namespace loop_sequencer {

/*************/
/* Utilities */
/*************/

void LoopSequencer::schedule(Requirements reqs, ActionFunc action) {
    schedule({{reqs, action}});
}

void LoopSequencer::schedule(
        std::vector<std::tuple<Requirements, ActionFunc>> list) {
    if (!list.empty()) {
        std::vector<std::tuple<Requirements, ActionFunc, ActionCheckFunc>>
                xlist;
        xlist.reserve(list.size());

        for (auto &entry : list)
            xlist.push_back(std::tuple_cat(
                    entry, std::make_tuple(ActionCheckFunc(nullptr))));

        schedule_if(xlist);
    }
}

void LoopSequencer::schedule_if(
        Requirements reqs, ActionFunc action, ActionCheckFunc check) {
    schedule_if({{reqs, action, check}});
}

void LoopSequencer::schedule_if(
        std::vector<std::tuple<Requirements, ActionFunc, ActionCheckFunc>>
                list) {
    if (!list.empty()) {
        validate(list);
        actions.push_back({list, NeverScheduled});
    }
}

void LoopSequencer::setCallback(CallbackType type, Callback cb) {
    callbacks[static_cast<size_t>(type)] = cb;
}

void LoopSequencer::setRemainderHandling(RemainderHandling handling) {
    remainderHandling = handling;
}

void LoopSequencer::callback(CallbackType type, int arg1, int arg2) {
    auto &cb = callbacks[static_cast<size_t>(type)];
    if (cb) cb(arg1, arg2);
}

void LoopSequencer::validate(
        std::vector<std::tuple<Requirements, ActionFunc, ActionCheckFunc>>
                &list) {
    if (list.empty()) throw std::runtime_error("No actions specified.");

    int variants = 1;
    int headPeriod = 0;

    auto &headReq = std::get<0>(list[0]);

    for (auto &action : list) {
        auto &req = std::get<0>(action);
        if (req.period <= 0) throw std::runtime_error("Invalid action period.");
        if (req.phase < 0 || req.phase >= req.period)
            throw std::runtime_error("Invalid action phase.");
        if (req.duration < 0 || req.duration + req.phase > req.period)
            throw std::runtime_error("Invalid action duration.");
        if (req.lookahead <= -req.period || req.lookahead > headReq.lookahead)
            throw std::runtime_error("Invalid action lookahead.");
        if (headPeriod == 0)
            headPeriod = req.period;
        else if (headPeriod % req.period)
            throw std::runtime_error(
                    "Backup action's period must evenly divide main action's "
                    "period.");
        if (req.variants > 0) variants = lcm(variants, req.variants);
        req.duration = std::max(req.duration, 1);
    }

    for (auto &action : list)
        std::get<0>(action).variants = variants;
}

void LoopSequencer::checkAnalyzed() const {
    if (!analyzed)
        throw std::runtime_error("Must call analyze() or materialize() first.");
}

int LoopSequencer::getUnroll() const {
    checkAnalyzed();
    return unroll;
}

int LoopSequencer::getWarmup() const {
    checkAnalyzed();
    return warmup;
}

int LoopSequencer::getCooldown() const {
    checkAnalyzed();
    return minCooldown;
}

/**************/
/* Main logic */
/**************/

// Analyze action list to determine unroll, warmup, and cooldown.
// Cooldown will be aligned to unroll.
void LoopSequencer::analyze() {
    if (analyzed) return;

    unroll = 1;
    maxLookahead = 0;
    minCooldown = 0;
    for (const auto &action : actions) {
        const auto &headReq = std::get<0>(action.list[0]);

        unroll = lcm(unroll, headReq.period * headReq.variants);
        maxLookahead = std::max(maxLookahead, headReq.lookahead);

        int mphase = (headReq.phase - headReq.lookahead) % headReq.period;
        if (mphase < 0) mphase += headReq.period;
        minCooldown = std::max(minCooldown,
                headReq.lookahead - headReq.period + mphase + headReq.duration);
    }

    minCooldown = ((minCooldown + unroll - 1) / unroll) * unroll;
    warmup = maxLookahead;
    analyzed = true;
}

// Sequence the loop.
void LoopSequencer::materialize(int maxLoops) {
    typedef CallbackType CT;

    analyze();

    resetActions();
    activeChecks.clear();
    currentBias = 0;

    bool unifyRemainder = (remainderHandling != RemainderHandling::Separate)
            && (minCooldown >= unroll) && (unroll > 1);
    int loopBias = minCooldown + unroll - 1;

    int labelShort, labelUnite;

    if (maxLoops > 0) {
        // Special path: completely unroll loop, handling up to maxLoop iterations.
        callback(CT::NotifyPhase, PhaseFullyUnrolled);
        int lMax = ((maxLoops + unroll - 1) / unroll) * unroll;
        for (int l = -warmup; l < lMax; l++)
            run(l, 0, maxLoops);
        closeChecks();
    } else {
        // Main path check: main path requires >= minCooldown iterations.
        if (minCooldown > 0)
            callback(CT::JumpIfLT, minCooldown, labelShort = nextLabel++);

        if (loopBias != 0) {
            currentBias += loopBias;
            callback(CT::OffsetCounter, -loopBias);
        }

        // Warmup.
        if (warmup > 0) callback(CT::NotifyPhase, PhaseWarmup);
        for (int l = -warmup; l < 0; l++)
            run(l, minCooldown);

        // Main loop.
        callback(CT::NotifyPhase, PhaseMainLoop);
        callback(CT::LoopStart, unroll);

        for (int l = 0; l < unroll; l++)
            run(l, unroll + minCooldown);

        callback(CT::LoopEnd, unroll);

        if (loopBias != 0) {
            currentBias -= loopBias;
            callback(CT::OffsetCounter, loopBias);
        }

        // Cooldown.
        //   - remaining loop count in interval [minCooldown, minCooldown + unroll)
        //   - if unifying remainder, just do minCooldown loops here and leave the rest for remainder.
        adjustActionTriggers(-unroll);
        callback(CT::NotifyPhase, PhaseCooldown);
        for (int l = 0;
                l < (unifyRemainder ? minCooldown : minCooldown + unroll); l++)
            run(l, minCooldown, minCooldown + unroll - 1);
        closeChecks();

        if (minCooldown > 1 && unifyRemainder)
            callback(CT::OffsetCounter, -minCooldown);

        callback(CT::NotifyPhase, PhaseMainPathEnd);

        if (minCooldown > 1) callback(CT::Jump, labelUnite = nextLabel++);

        // Short loop.
        if (minCooldown > 0) {
            callback(CT::JumpTarget, labelShort);
            callback(CT::NotifyPhase, PhaseShortLoop);
        }

        if (minCooldown > 1) {
            resetActions();

            if (unifyRemainder) {
                // If unifying remainder, group loops into chunks of size unroll.
                for (int l = -warmup; l < 0; l++)
                    run(l, 0, minCooldown - 1);

                int labelNoChunks = nextLabel++;

                for (int l0 = 0; l0 < (minCooldown - unroll); l0 += unroll) {
                    int chunk = std::min(unroll, minCooldown - l0 - unroll);
                    bool needCheck = precheck(chunk);

                    if (needCheck) callback(CT::JumpIfLT, chunk, labelNoChunks);
                    for (int l = 0; l < chunk; l++)
                        run(l, chunk, minCooldown - 1 - l0);
                    callback(CT::OffsetCounter, -chunk);
                    adjustActionTriggers(-chunk);
                    closeChecks();
                }
                if (minCooldown > unroll)
                    callback(CT::JumpTarget, labelNoChunks);
            } else {
                for (int l = -warmup; l < minCooldown; l++)
                    run(l, 0, minCooldown - 1);
            }

            closeChecks();
            callback(CT::JumpTarget, labelUnite);
        }

        // Unified remainder handling. Loop count is unbiased on all paths.
        // TODO: is it always safe to unify main/short remainders when there are actions
        //   whose backups have different lookahead?
        if (unifyRemainder
                && (remainderHandling != RemainderHandling::Ignore)) {
            callback(CT::NotifyPhase, PhaseRemainder);
            for (int l = 0; l < unroll - 1; l++)
                run(l, 0, unroll - 1);
            closeChecks();
        }
    }

    nextLabel = 0;
}

void LoopSequencer::run(int l, int guaranteedMin, int guaranteedMax) {
    typedef CallbackType CT;

    for (auto &action : actions) {
        const auto &list = action.list;

        // Find the first item in the list that matches trigger criteria (if any) and run it.
        for (size_t i = 0; i < list.size(); i++) {
            const auto &item = list[i];
            const auto &req = std::get<0>(item);
            const auto &execute = std::get<1>(item);
            const auto &check = std::get<2>(item);
            int lTrigger = l + req.lookahead;
            int minLoops = lTrigger + req.duration;
            bool lastResort = (i + 1 == list.size());

            if (lTrigger < 0) break;

            if ((lTrigger + req.period) % req.period == req.phase) {
                // Skip if this action can never be triggered.
                if (guaranteedMax >= 0 && minLoops > guaranteedMax) continue;

                // Skip if this action may not be triggered, and there's a backup plan.
                if (minLoops > guaranteedMin && !lastResort) continue;

                // Skip if this action's trigger falls within an already-covered section of iteration space.
                if (lTrigger < action.nextTrigger) continue;

                // Check if this action has work to do.
                Iteration iteration(lTrigger,
                        std::max(0, guaranteedMin - lTrigger),
                        currentBias - lTrigger);
                if (!check || check(iteration)) {
                    bool unconditional = (req.checkType == Unconditional);
                    bool optionalCheck = (req.checkType == OptionalCheck);

                    if (!optionalCheck) {
                        // If no loop count check desired for this action, pretend it doesn't need any loops.
                        if (unconditional) minLoops = 0;

                        // Finish all active checks > minLoops.
                        // If minLoops not currently being checked, then add check.
                        int thresh = minLoops;
                        bool needCheck = precheck(thresh) & !unconditional;

                        if (guaranteedMin < minLoops && needCheck) {
                            int label = nextLabel++;
                            callback(CT::JumpIfLT, thresh, label);
                            activeChecks.push_back(
                                    std::make_pair(thresh, label));
                        }
                    }

                    execute(iteration);
                }

                action.nextTrigger = lTrigger - req.phase + req.period;
                break;
            } else {
                // Find when this item will be triggered in this period.
                lTrigger = lTrigger - (lTrigger % req.period) + req.phase;
                minLoops = lTrigger + req.duration;

                // If it is guaranteed to be triggered, then don't consider backups.
                if (minLoops <= guaranteedMin) break;
            }
        }
    }
}

void LoopSequencer::closeChecks() {
    for (const auto &val : activeChecks)
        callback(CallbackType::JumpTarget, val.second);
    activeChecks.clear();
}

bool LoopSequencer::precheck(int thresh) {
    bool alreadyChecked = false;

    for (auto iter = activeChecks.begin(); iter < activeChecks.end();) {
        int thisThresh = iter->first;
        int thisLabel = iter->second;

        if (thisThresh > thresh) {
            callback(CallbackType::JumpTarget, thisLabel);
            iter = activeChecks.erase(iter);
        } else {
            alreadyChecked |= (thisThresh == thresh);
            iter++;
        }
    }

    return !alreadyChecked;
}

void LoopSequencer::resetActions() {
    for (auto &action : actions)
        action.nextTrigger = NeverScheduled;
}

void LoopSequencer::adjustActionTriggers(int shift) {
    for (auto &action : actions)
        if (action.nextTrigger != NeverScheduled) action.nextTrigger += shift;
}

} /* namespace loop_sequencer */

} // namespace jit
} // namespace gpu
} // namespace impl
} // namespace dnnl
