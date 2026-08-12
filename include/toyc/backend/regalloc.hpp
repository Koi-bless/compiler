#pragma once
#include <optional>
#include <vector>
#include "toyc/backend/liveness.hpp"
namespace toyc {
struct RegAllocOptions {
    bool enableCopyHints = false;
    bool enableRematerialization = false;
};
struct Rematerialization {
    MOpcode opcode = MOpcode::LI;
    Immediate immediate{};
};
struct AllocationResult {
    std::vector<std::optional<PhysReg>> registers;
    std::vector<std::optional<StackSlot>> spillSlots;
    std::vector<PhysReg> usedCalleeSaved;
    std::vector<std::optional<Rematerialization>> rematerializations;
};
class LinearScanRegisterAllocator {
public:
    explicit LinearScanRegisterAllocator(RegAllocOptions options = {})
        : options_(options) {}
    AllocationResult run(MachineFunction& function) const;
private:
    RegAllocOptions options_;
};
}
