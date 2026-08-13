#pragma once

#include "toyc/opt/pass.hpp"

namespace toyc {

// Clone small non-recursive callees, including functions with CFG cycles.  The
// module budget and call-graph cycle check bound code growth.
PassResult runFunctionInlining(IRModule& module, std::size_t growthBudget = 2048,
                               std::size_t calleeInstructionLimit = 192);

} // namespace toyc
