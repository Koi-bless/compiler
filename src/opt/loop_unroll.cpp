#include "toyc/opt/loop_unroll.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include "toyc/opt/ir_utils.hpp"
#include "toyc/opt/loop_analysis.hpp"
#include "toyc/opt/loop_summary.hpp"

namespace toyc {
namespace {

struct HeaderState {
    ValueId value{};
    ValueId initial{};
    ValueId backedge{};
};

struct UnrollPlan {
    CountedLoopSummary counted;
    BlockId header{};
    BlockId bodyEntry{};
    std::vector<BlockId> blocks;
    std::vector<HeaderState> states;
    std::vector<BlockId> definitionBlocks;
    std::size_t growth{};
};

std::optional<UnrollPlan> planUnroll(const IRFunction& function,
                                     const Loop& loop,
                                     std::uint64_t maxTrips,
                                     std::size_t budget) {
    const auto counted = summarizeCountedLoop(function, loop);
    if (!counted || counted->trips == 0 || counted->trips > maxTrips ||
        !loop.preheader || loop.latches.size() != 1)
        return std::nullopt;

    const std::set<BlockId> members(loop.blocks.begin(), loop.blocks.end());
    const auto* preheaderJump = std::get_if<IRJump>(
        &*function.blocks[*loop.preheader].terminator);
    const auto* headerBranch = std::get_if<BranchValue>(
        &*function.blocks[loop.header].terminator);
    const auto* latchJump = std::get_if<IRJump>(
        &*function.blocks[counted->latch].terminator);
    if (!preheaderJump || preheaderJump->target != loop.header ||
        !headerBranch || !latchJump || latchJump->target != loop.header)
        return std::nullopt;

    const bool trueInside = members.contains(headerBranch->trueTarget);
    const bool falseInside = members.contains(headerBranch->falseTarget);
    if (trueInside == falseInside) return std::nullopt;
    const BlockId bodyEntry = trueInside ? headerBranch->trueTarget
                                         : headerBranch->falseTarget;
    const BlockId exit = trueInside ? headerBranch->falseTarget
                                    : headerBranch->trueTarget;
    if (exit != counted->exit) return std::nullopt;

    const std::set<BlockId> expectedHeaderPredecessors{
        *loop.preheader, counted->latch};
    if (std::set<BlockId>(function.blocks[loop.header].predecessors.begin(),
                          function.blocks[loop.header].predecessors.end()) !=
        expectedHeaderPredecessors)
        return std::nullopt;

    for (const BlockId blockId : loop.blocks) {
        if (blockId == loop.header) continue;
        const auto& block = function.blocks[blockId];
        if (std::any_of(block.predecessors.begin(), block.predecessors.end(),
                        [&](BlockId predecessor) {
                            return !members.contains(predecessor);
                        }))
            return std::nullopt;
        if (std::holds_alternative<ReturnValue>(*block.terminator) ||
            std::holds_alternative<IRUnreachable>(*block.terminator))
            return std::nullopt;
        for (const BlockId successor : block.successors)
            if (!members.contains(successor)) return std::nullopt;
    }

    std::vector<BlockId> definitions(function.valueCount, function.entry);
    std::size_t instructionsPerIteration = 0;
    std::size_t finalHeaderInstructions = 0;
    for (const auto& block : function.blocks) {
        for (const auto& instruction : block.instructions) {
            if (instruction.result) definitions[*instruction.result] = block.id;
            if (!members.contains(block.id) ||
                (block.id == loop.header && instruction.op == IROp::Phi))
                continue;
            ++instructionsPerIteration;
            if (block.id == loop.header) ++finalHeaderInstructions;
        }
    }
    const std::uint64_t growth64 =
        static_cast<std::uint64_t>(instructionsPerIteration) * counted->trips +
        finalHeaderInstructions;
    if (growth64 > budget) return std::nullopt;

    std::vector<HeaderState> states;
    for (const auto& instruction : function.blocks[loop.header].instructions) {
        if (instruction.op != IROp::Phi) break;
        if (!instruction.result) return std::nullopt;
        std::optional<ValueId> initial;
        std::optional<ValueId> backedge;
        for (const auto& input : instruction.phiInputs) {
            if (input.predecessor == *loop.preheader) initial = input.value;
            if (input.predecessor == counted->latch) backedge = input.value;
        }
        if (!initial || !backedge) return std::nullopt;
        states.push_back({*instruction.result, *initial, *backedge});
    }

    for (const auto& instruction : function.blocks[exit].instructions) {
        if (instruction.op != IROp::Phi) break;
        const auto input = std::find_if(
            instruction.phiInputs.begin(), instruction.phiInputs.end(),
            [&](const PhiInput& candidate) {
                return candidate.predecessor == loop.header;
            });
        if (input == instruction.phiInputs.end()) return std::nullopt;
        if (members.contains(definitions[input->value]) &&
            definitions[input->value] != loop.header)
            return std::nullopt;
    }

    std::vector<BlockId> blocks = loop.blocks;
    std::sort(blocks.begin(), blocks.end());
    return UnrollPlan{*counted, loop.header, bodyEntry, std::move(blocks),
                      std::move(states), std::move(definitions),
                      static_cast<std::size_t>(growth64)};
}

ValueId remapValue(ValueId value, const UnrollPlan& plan,
                   const std::set<BlockId>& members,
                   const std::map<ValueId, ValueId>& values) {
    if (!members.contains(plan.definitionBlocks.at(value))) return value;
    return values.at(value);
}

std::map<ValueId, ValueId> appendIteration(
    IRFunction& function, const UnrollPlan& plan,
    const std::set<BlockId>& members,
    const std::map<ValueId, ValueId>& stateValues,
    std::map<BlockId, BlockId>& blockMap) {
    std::map<ValueId, ValueId> values = stateValues;
    for (const BlockId source : plan.blocks) {
        const BlockId destination = static_cast<BlockId>(function.blocks.size());
        blockMap.emplace(source, destination);
        IRBlock block;
        block.id = destination;
        function.blocks.push_back(std::move(block));
    }

    for (const BlockId source : plan.blocks)
        for (const auto& instruction : function.blocks[source].instructions) {
            if (source == plan.header && instruction.op == IROp::Phi) continue;
            if (instruction.result)
                values.emplace(*instruction.result, function.valueCount++);
        }

    for (const BlockId source : plan.blocks) {
        auto& destination = function.blocks[blockMap.at(source)];
        for (const auto& original : function.blocks[source].instructions) {
            if (source == plan.header && original.op == IROp::Phi) continue;
            IRInstruction clone = original;
            clone.id = function.instructionCount++;
            if (clone.result) clone.result = values.at(*clone.result);
            for (auto& operand : clone.operands)
                operand = remapValue(operand, plan, members, values);
            for (auto& input : clone.phiInputs) {
                input.predecessor = blockMap.at(input.predecessor);
                input.value = remapValue(input.value, plan, members, values);
            }
            destination.instructions.push_back(std::move(clone));
        }
        if (source == plan.header) {
            destination.terminator = IRJump{blockMap.at(plan.bodyEntry)};
        } else if (const auto* jump = std::get_if<IRJump>(
                       &*function.blocks[source].terminator)) {
            destination.terminator = IRJump{blockMap.at(jump->target)};
        } else if (const auto* branch = std::get_if<BranchValue>(
                       &*function.blocks[source].terminator)) {
            destination.terminator = BranchValue{
                remapValue(branch->condition, plan, members, values),
                blockMap.at(branch->trueTarget),
                blockMap.at(branch->falseTarget)};
        }
    }
    return values;
}

void applyUnroll(IRFunction& function, const Loop& loop,
                 const UnrollPlan& plan) {
    IRFunction transformed = function;
    const std::set<BlockId> members(loop.blocks.begin(), loop.blocks.end());
    std::map<ValueId, ValueId> stateValues;
    for (const auto& state : plan.states)
        stateValues.emplace(state.value, state.initial);

    std::optional<BlockId> firstHeader;
    std::optional<BlockId> priorLatch;
    for (std::uint64_t iteration = 0; iteration < plan.counted.trips;
         ++iteration) {
        std::map<BlockId, BlockId> blockMap;
        const auto values = appendIteration(
            transformed, plan, members, stateValues, blockMap);
        const BlockId header = blockMap.at(plan.header);
        const BlockId latch = blockMap.at(plan.counted.latch);
        if (!firstHeader) firstHeader = header;
        if (priorLatch)
            transformed.blocks[*priorLatch].terminator = IRJump{header};
        priorLatch = latch;
        for (const auto& state : plan.states)
            stateValues[state.value] = remapValue(
                state.backedge, plan, members, values);
    }

    const BlockId finalHeader = static_cast<BlockId>(transformed.blocks.size());
    IRBlock bridge;
    bridge.id = finalHeader;
    std::map<ValueId, ValueId> finalValues = stateValues;
    for (const auto& instruction : function.blocks[plan.header].instructions) {
        if (instruction.op == IROp::Phi) continue;
        if (instruction.result)
            finalValues.emplace(*instruction.result, transformed.valueCount++);
    }
    for (const auto& original : function.blocks[plan.header].instructions) {
        if (original.op == IROp::Phi) continue;
        IRInstruction clone = original;
        clone.id = transformed.instructionCount++;
        if (clone.result) clone.result = finalValues.at(*clone.result);
        for (auto& operand : clone.operands)
            operand = remapValue(operand, plan, members, finalValues);
        bridge.instructions.push_back(std::move(clone));
    }
    bridge.terminator = IRJump{plan.counted.exit};
    transformed.blocks.push_back(std::move(bridge));

    transformed.blocks[*loop.preheader].terminator = IRJump{*firstHeader};
    transformed.blocks[*priorLatch].terminator = IRJump{finalHeader};
    for (auto& instruction : transformed.blocks[plan.counted.exit].instructions) {
        if (instruction.op != IROp::Phi) break;
        const auto input = std::find_if(
            instruction.phiInputs.begin(), instruction.phiInputs.end(),
            [&](const PhiInput& candidate) {
                return candidate.predecessor == plan.header;
            });
        const ValueId exitValue = remapValue(
            input->value, plan, members, finalValues);
        instruction.phiInputs.push_back({finalHeader, exitValue});
    }

    // Header values can be live beyond the immediate exit block, for example
    // as the backedge input of an enclosing loop. Redirect every such use to
    // the state computed after the final expanded iteration. The old loop is
    // unreachable and is removed immediately below.
    for (const auto& instruction : function.blocks[plan.header].instructions)
        if (instruction.result)
            replaceAllUses(transformed, *instruction.result,
                           finalValues.at(*instruction.result));

    canonicalizeIR(transformed);
    function = std::move(transformed);
}

} // namespace

PassResult runSmallLoopUnroll(IRFunction& function, std::uint64_t maxTrips,
                              std::size_t growthBudget) {
    PassResult result;
    std::size_t remaining = growthBudget;
    while (remaining != 0) {
        auto loops = analyzeLoops(function);
        std::stable_sort(loops.begin(), loops.end(),
            [](const Loop& lhs, const Loop& rhs) { return lhs.depth > rhs.depth; });
        bool changed = false;
        for (const auto& loop : loops) {
            const std::set<BlockId> members(loop.blocks.begin(),
                                            loop.blocks.end());
            const bool containsNestedLoop = std::any_of(
                loops.begin(), loops.end(), [&](const Loop& candidate) {
                    return candidate.depth > loop.depth &&
                        members.contains(candidate.header);
                });
            if (containsNestedLoop) continue;
            const auto plan = planUnroll(function, loop, maxTrips, remaining);
            if (!plan) continue;
            applyUnroll(function, loop, *plan);
            remaining -= plan->growth;
            result.changed = true;
            result.instructionsReplaced += plan->growth;
            changed = true;
            break;
        }
        if (!changed) break;
    }
    return result;
}

} // namespace toyc
