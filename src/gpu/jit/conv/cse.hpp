/*******************************************************************************
* Copyright 2022 Intel Corporation
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

#ifndef GPU_JIT_CONV_CSE_HPP
#define GPU_JIT_CONV_CSE_HPP

#include "gpu/jit/conv/config.hpp"
#include "gpu/jit/conv/ir.hpp"

namespace dnnl {
namespace impl {
namespace gpu {
namespace jit {

// Pass for common subexpression elimination (CSE).
stmt_t eliminate_common_subexprs(
        const stmt_t &_stmt, const conv_config_t &cfg, ir_context_t &ir_ctx);

} // namespace jit
} // namespace gpu
} // namespace impl
} // namespace dnnl

#endif
