#pragma once
#include <cstdint>
#include <set>
#include <vector>
#include "toyc/backend/mir.hpp"
namespace toyc {
struct BlockLiveness {
    std::set<VRegId> use, def, liveIn, liveOut;
};
struct LiveInterval {
    VRegId vreg{};
    std::uint32_t start{};
    std::uint32_t end{};
    std::vector<std::uint32_t> uses;
    bool crossesCall = false;
    double spillWeight = 0.0;
};
struct LivenessResult {
    std::vector<BlockLiveness> blocks;
    std::vector<LiveInterval> intervals;
};
LivenessResult computeLiveness(const MachineFunction& function);
}
