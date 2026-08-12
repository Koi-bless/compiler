#pragma once

#include "toyc/backend/machine_pass.hpp"
#include "toyc/backend/mir.hpp"

namespace toyc {

struct MachinePeepholeOptions {
    bool removeSelfCopies = true;
    bool removeOverwrittenDefinitions = true;
    bool forwardStackSlots = true;
    bool eliminateLocalDeadSpillStores = true;
};

MachinePassResult runPostRAPeephole(
    MachineFunction& function,
    const MachinePeepholeOptions& options = {});

} // namespace toyc
