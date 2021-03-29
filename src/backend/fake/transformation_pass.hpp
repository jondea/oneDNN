/*******************************************************************************
* Copyright 2021 Intel Corporation
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

#ifndef BACKEND_FAKE_TRANSFORMATION_PASS_HPP
#define BACKEND_FAKE_TRANSFORMATION_PASS_HPP

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "interface/pass_base.hpp"

#include "pattern_utils.hpp"

namespace dnnl {
namespace graph {
namespace impl {
namespace fake_impl {
namespace pass {

/*!
 * \brief transformation_pass generates an optimized graph
 *        when the pass is hit, it can be op replacements,
 *        dead branch elimination, etc.
 */
class transformation_pass : public impl::pass::pass_base {
public:
    explicit transformation_pass(std::string pbackend, std::string pname)
        : impl::pass::pass_base(impl::pass::pass_type::kTransformation,
                std::move(pbackend), std::move(pname)) {}

    static impl::pass::pass_base_ptr create(
            std::string pbackend, std::string pname) {
        return std::make_shared<transformation_pass>(
                std::move(pbackend), std::move(pname));
    }

    // the criteria of pass execution
    void run(impl::graph_t &agraph) override {
        std::vector<op_t *> fusion_ops;
        pattern_utils pu;
        pu.match(agraph, fusion_ops);
        if (!fusion_ops.empty()) {
            // temporary solution here for showing which pattern matched
            char *val = std::getenv("DNNL_GRAPH_DUMP");
            if (val != nullptr && std::strcmp(val, "1") == 0) {
                std::cout << "hit pass " << get_pass_name() << "\n";
            }

            // Only fuse not rewrite. Will remove the fuse once dnnl
            // backend support subgraph mode
            pu.fuse(agraph, fusion_ops);
        }
    }
};

#define DECLARE_PASS_EX(bname, pname, counter) \
    static auto _registered_pass_##pname##_##bname##_##counter##_

#define DECLARE_PASS(bname, pname, counter) \
    DECLARE_PASS_EX(bname, pname, counter)

#define FAKE_BACKEND_REGISTER_TRANSFORMATION_PASS( \
        backend_name, pass_class_name) \
    DECLARE_PASS(backend_name, pass_class_name, __COUNTER__) \
            = registry.register_pass(#backend_name, #pass_class_name, \
                    &transformation_pass::create)

#define FAKE_BACKEND_REGISTER_PASSES_DEF_BEGIN(passes_class_) \
    void register_##passes_class_(impl::pass::pass_registry &registry) {
#define FAKE_BACKEND_REGISTER_PASSES_DEF_END }

#define FAKE_BACKEND_REGISTER_PASSES_CALL(passes_class_, pass_registry_) \
    pass::register_##passes_class_(pass_registry_);

} // namespace pass
} // namespace fake_impl
} // namespace impl
} // namespace graph
} // namespace dnnl

#endif
