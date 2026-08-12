#pragma once

#include <cstddef>

namespace toyc {

struct MachinePassResult {
    bool changed = false;
    std::size_t instructionsRewritten = 0;
    std::size_t instructionsRemoved = 0;
    std::size_t loadsForwarded = 0;
    std::size_t storesRemoved = 0;

    MachinePassResult& operator+=(const MachinePassResult& other) {
        changed = changed || other.changed;
        instructionsRewritten += other.instructionsRewritten;
        instructionsRemoved += other.instructionsRemoved;
        loadsForwarded += other.loadsForwarded;
        storesRemoved += other.storesRemoved;
        return *this;
    }
};

} // namespace toyc
