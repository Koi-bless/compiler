#pragma once
#include <optional>
#include <vector>
#include "toyc/backend/liveness.hpp"
namespace toyc {
struct AllocationResult {
    std::vector<std::optional<PhysReg>> registers;
    std::vector<std::optional<StackSlot>> spillSlots;
    std::vector<PhysReg> usedCalleeSaved;
};
class LinearScanRegisterAllocator {
public:
    AllocationResult run(MachineFunction& function) const;
};
}
