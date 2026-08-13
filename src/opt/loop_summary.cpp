#include "toyc/opt/loop_summary.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <set>

#include "toyc/opt/ir_utils.hpp"

namespace toyc {
namespace {

const IRInstruction* definitionIgnoringCopies(const IRFunction& function,
                                              ValueId value) {
    for (std::uint32_t depth = 0; depth <= function.valueCount; ++depth) {
        const auto* definition = findDefinition(function, value);
        if (!definition || definition->op != IROp::Copy ||
            definition->operands.size() != 1)
            return definition;
        value = definition->operands[0];
    }
    return nullptr;
}

std::optional<std::int32_t> constantValue(const IRFunction& function,
                                          ValueId value) {
    const auto* definition = definitionIgnoringCopies(function, value);
    if (!definition || definition->op != IROp::Constant || !definition->immediate)
        return std::nullopt;
    return definition->immediate;
}

LoopPredicate swapped(LoopPredicate predicate) {
    switch (predicate) {
    case LoopPredicate::LT: return LoopPredicate::GT;
    case LoopPredicate::LE: return LoopPredicate::GE;
    case LoopPredicate::GT: return LoopPredicate::LT;
    case LoopPredicate::GE: return LoopPredicate::LE;
    }
    return predicate;
}

LoopPredicate inverted(LoopPredicate predicate) {
    switch (predicate) {
    case LoopPredicate::LT: return LoopPredicate::GE;
    case LoopPredicate::LE: return LoopPredicate::GT;
    case LoopPredicate::GT: return LoopPredicate::LE;
    case LoopPredicate::GE: return LoopPredicate::LT;
    }
    return predicate;
}

std::optional<LoopPredicate> predicateFor(IROp op) {
    switch (op) {
    case IROp::ICmpLT: return LoopPredicate::LT;
    case IROp::ICmpLE: return LoopPredicate::LE;
    case IROp::ICmpGT: return LoopPredicate::GT;
    case IROp::ICmpGE: return LoopPredicate::GE;
    default: return std::nullopt;
    }
}

std::optional<std::uint64_t> tripCount(std::int32_t initial,
                                       std::int32_t bound,
                                       std::int32_t step,
                                       LoopPredicate predicate) {
    const std::int64_t start = initial;
    const std::int64_t limit = bound;
    const std::int64_t stride = step;
    std::int64_t count = 0;
    switch (predicate) {
    case LoopPredicate::LT:
        if (stride <= 0) return std::nullopt;
        if (start < limit) count = (limit - start + stride - 1) / stride;
        break;
    case LoopPredicate::LE:
        if (stride <= 0) return std::nullopt;
        if (start <= limit) count = (limit - start) / stride + 1;
        break;
    case LoopPredicate::GT: {
        if (stride >= 0) return std::nullopt;
        const std::int64_t magnitude = -stride;
        if (start > limit) count = (start - limit + magnitude - 1) / magnitude;
        break;
    }
    case LoopPredicate::GE: {
        if (stride >= 0) return std::nullopt;
        const std::int64_t magnitude = -stride;
        if (start >= limit) count = (start - limit) / magnitude + 1;
        break;
    }
    }
    if (count < 0) return std::nullopt;
    return static_cast<std::uint64_t>(count);
}

} // namespace

std::optional<CountedLoopSummary> summarizeCountedLoop(
    const IRFunction& function, const Loop& loop) {
    if (!loop.preheader || loop.latches.size() != 1) return std::nullopt;
    const auto& header = function.blocks[loop.header];
    if (!header.terminator) return std::nullopt;
    const auto* branch = std::get_if<BranchValue>(&*header.terminator);
    if (!branch) return std::nullopt;
    const std::set<BlockId> members(loop.blocks.begin(), loop.blocks.end());
    const bool trueInside = members.contains(branch->trueTarget);
    const bool falseInside = members.contains(branch->falseTarget);
    if (trueInside == falseInside) return std::nullopt;
    const BlockId exit = trueInside ? branch->falseTarget : branch->trueTarget;
    const auto* comparison = findDefinition(function, branch->condition);
    if (!comparison || comparison->operands.size() != 2) return std::nullopt;
    auto predicate = predicateFor(comparison->op);
    if (!predicate) return std::nullopt;
    if (!trueInside) *predicate = inverted(*predicate);

    for (const auto& phi : header.instructions) {
        if (phi.op != IROp::Phi) break;
        if (!phi.result || phi.phiInputs.size() != 2) continue;
        std::optional<ValueId> initialValue;
        std::optional<ValueId> backedgeValue;
        for (const auto& input : phi.phiInputs) {
            if (input.predecessor == *loop.preheader) initialValue = input.value;
            if (input.predecessor == loop.latches[0]) backedgeValue = input.value;
        }
        if (!initialValue || !backedgeValue) continue;
        ValueId boundValue{};
        auto inductionPredicate = *predicate;
        if (comparison->operands[0] == *phi.result) {
            boundValue = comparison->operands[1];
        } else if (comparison->operands[1] == *phi.result) {
            boundValue = comparison->operands[0];
            inductionPredicate = swapped(inductionPredicate);
        } else {
            continue;
        }
        const auto initial = constantValue(function, *initialValue);
        const auto bound = constantValue(function, boundValue);
        if (!initial || !bound) continue;
        const auto* update = definitionIgnoringCopies(function, *backedgeValue);
        if (!update || update->operands.size() != 2) continue;
        std::optional<std::int32_t> step;
        if (update->op == IROp::Add) {
            if (update->operands[0] == *phi.result)
                step = constantValue(function, update->operands[1]);
            else if (update->operands[1] == *phi.result)
                step = constantValue(function, update->operands[0]);
        } else if (update->op == IROp::Sub &&
                   update->operands[0] == *phi.result) {
            if (const auto amount = constantValue(function, update->operands[1])) {
                const std::int64_t negated = -static_cast<std::int64_t>(*amount);
                if (negated >= std::numeric_limits<std::int32_t>::min() &&
                    negated <= std::numeric_limits<std::int32_t>::max())
                    step = static_cast<std::int32_t>(negated);
            }
        }
        if (!step || *step == 0) continue;
        const auto trips = tripCount(*initial, *bound, *step, inductionPredicate);
        if (!trips) continue;
        const std::int64_t exitValue = static_cast<std::int64_t>(*initial) +
            static_cast<std::int64_t>(*trips) * static_cast<std::int64_t>(*step);
        const std::int64_t last = *trips == 0 ? *initial :
            static_cast<std::int64_t>(*initial) +
                static_cast<std::int64_t>(*trips - 1) * static_cast<std::int64_t>(*step);
        if (exitValue < std::numeric_limits<std::int32_t>::min() ||
            exitValue > std::numeric_limits<std::int32_t>::max() ||
            last < std::numeric_limits<std::int32_t>::min() ||
            last > std::numeric_limits<std::int32_t>::max())
            continue;
        return CountedLoopSummary{
            loop.latches[0], exit, *phi.result, *initialValue, boundValue,
            *initial, *bound, *step, *trips, static_cast<std::int32_t>(last),
            static_cast<std::int32_t>(exitValue), inductionPredicate};
    }
    return std::nullopt;
}

} // namespace toyc
