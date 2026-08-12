#pragma once

#include <cstdint>

namespace toyc::riscv32 {

inline constexpr std::int64_t stackAlignment = 16;
inline constexpr std::int64_t wordSize = 4;
inline constexpr std::int64_t immediateMin = -2048;
inline constexpr std::int64_t immediateMax = 2047;
inline constexpr unsigned argumentRegisterCount = 8;
inline constexpr unsigned shiftAmountBits = 5;

inline constexpr bool fitsImmediate(std::int64_t value) {
    return value >= immediateMin && value <= immediateMax;
}

inline constexpr bool fitsShiftAmount(std::int64_t value) {
    return value >= 0 && value < (std::int64_t{1} << shiftAmountBits);
}

[[nodiscard]] constexpr std::int64_t alignUp(std::int64_t value,
                                             std::int64_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

} // namespace toyc::riscv32
