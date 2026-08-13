#pragma once

#include <cstdint>
#include <optional>

#include "toyc/opt/loop_analysis.hpp"

namespace toyc {

enum class LoopPredicate { LT, LE, GT, GE, EQ, NE };

// A canonical counted loop whose induction values and trip count are exact.
// Keeping this analysis separate lets transformations and effect analysis share
// the same overflow and termination proof.
struct CountedLoopSummary {
    BlockId latch{};
    BlockId exit{};
    ValueId induction{};
    ValueId initial{};
    ValueId bound{};
    std::int32_t initialConstant{};
    std::int32_t boundConstant{};
    std::int32_t step{};
    std::uint64_t trips{};
    std::int32_t last{};
    std::int32_t exitValue{};
    LoopPredicate predicate = LoopPredicate::LT;
};

std::optional<CountedLoopSummary> summarizeCountedLoop(
    const IRFunction& function, const Loop& loop);

} // namespace toyc
