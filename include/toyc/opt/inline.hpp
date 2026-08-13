#pragma once

#include "toyc/opt/pass.hpp"

namespace toyc {

// Clone small acyclic callees, including branches and multiple returns.  The
// module budget and call-graph cycle check bound code growth.
PassResult runFunctionInlining(IRModule& module, std::size_t growthBudget = 512,
                               std::size_t calleeInstructionLimit = 64);

} // namespace toyc
