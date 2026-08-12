#pragma once

#include "toyc/backend/machine_pass.hpp"
#include "toyc/backend/mir.hpp"

namespace toyc {

struct MachineCombineOptions {
    bool combineImmediates = true;
    bool useZeroRegister = true;
    bool reducePowerOfTwoMultiply = true;
    bool propagateVirtualCopies = true;
    bool eliminateDeadDefinitions = true;
};

MachinePassResult runPreRAMachineCombine(
    MachineFunction& function,
    const MachineCombineOptions& options = {});

} // namespace toyc
