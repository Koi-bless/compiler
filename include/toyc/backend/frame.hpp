#pragma once
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>
#include "toyc/backend/mir.hpp"
namespace toyc {
struct FrameLayout {
    std::int32_t frameSize{};
    std::int32_t outgoingBytes{};
    std::vector<std::int32_t> spillOffsets;
    std::vector<std::pair<PhysReg, std::int32_t>> calleeSavedOffsets;
    std::optional<std::int32_t> raOffset;
};
class FrameLowering {
public:
    FrameLayout run(MachineFunction& function) const;
};
}
