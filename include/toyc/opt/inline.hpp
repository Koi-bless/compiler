#pragma once

#include "toyc/opt/pass.hpp"

namespace toyc {

// Inlines small, single-block leaf functions.  Keeping this first version
// deliberately narrow avoids CFG cloning while still exposing pure expression
// helpers to scalar and loop optimization.
PassResult runFunctionInlining(IRModule& module, std::size_t growthBudget = 256,
                               std::size_t calleeInstructionLimit = 24);

} // namespace toyc
