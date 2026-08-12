#include "toyc/opt/sccp.hpp"

#include <algorithm>
#include <set>
#include <utility>
#include <vector>

#include "toyc/opt/ir_utils.hpp"

namespace toyc {
namespace {

enum class LatticeKind { Unknown, Constant, Overdefined };

struct LatticeValue {
    LatticeKind kind = LatticeKind::Unknown;
    std::int32_t constant = 0;
};

LatticeValue meet(LatticeValue lhs, LatticeValue rhs) {
    if (lhs.kind == LatticeKind::Unknown) return rhs;
    if (rhs.kind == LatticeKind::Unknown) return lhs;
    if (lhs.kind == LatticeKind::Overdefined || rhs.kind == LatticeKind::Overdefined)
        return {LatticeKind::Overdefined, 0};
    if (lhs.constant == rhs.constant) return lhs;
    return {LatticeKind::Overdefined, 0};
}

bool update(LatticeValue& destination, LatticeValue next) {
    const LatticeValue merged = meet(destination, next);
    if (merged.kind == destination.kind &&
        (merged.kind != LatticeKind::Constant || merged.constant == destination.constant))
        return false;
    destination = merged;
    return true;
}

LatticeValue evaluate(const IRInstruction& instruction,
                      const std::vector<LatticeValue>& values) {
    switch (instruction.op) {
    case IROp::Param: case IROp::LoadGlobal: case IROp::Call:
        return {LatticeKind::Overdefined, 0};
    case IROp::Constant:
        return {LatticeKind::Constant, *instruction.immediate};
    case IROp::Copy:
        return values[instruction.operands[0]];
    case IROp::Phi:
        return {};
    case IROp::LogicalNot: {
        const auto operand = values[instruction.operands[0]];
        if (operand.kind == LatticeKind::Overdefined) return operand;
        if (operand.kind == LatticeKind::Unknown) return operand;
        return {LatticeKind::Constant, *foldUnary(instruction.op, operand.constant)};
    }
    case IROp::Add: case IROp::Sub: case IROp::Mul: case IROp::SDiv:
    case IROp::SRem: case IROp::ICmpLT: case IROp::ICmpGT:
    case IROp::ICmpLE: case IROp::ICmpGE: case IROp::ICmpEQ: case IROp::ICmpNE: {
        const auto lhs = values[instruction.operands[0]];
        const auto rhs = values[instruction.operands[1]];
        if (lhs.kind == LatticeKind::Overdefined || rhs.kind == LatticeKind::Overdefined)
            return {LatticeKind::Overdefined, 0};
        if (lhs.kind == LatticeKind::Unknown || rhs.kind == LatticeKind::Unknown)
            return {};
        if (const auto folded = foldBinary(instruction.op, lhs.constant, rhs.constant))
            return {LatticeKind::Constant, *folded};
        return {LatticeKind::Overdefined, 0};
    }
    case IROp::StoreGlobal:
        return {};
    }
    return {};
}

} // namespace

PassResult runSCCP(IRFunction& function) {
    std::vector<LatticeValue> values(function.valueCount);
    std::vector<bool> executable(function.blocks.size(), false);
    std::set<std::pair<BlockId, BlockId>> edges;
    executable[function.entry] = true;

    bool changed = true;
    unsigned iterations = 0;
    while (changed) {
        changed = false;
        if (++iterations > function.valueCount + function.blocks.size() * 4U + 16U)
            break;
        for (const auto& block : function.blocks) if (executable[block.id]) {
            for (const auto& instruction : block.instructions) {
                if (!instruction.result) continue;
                LatticeValue next;
                if (instruction.op == IROp::Phi) {
                    for (const auto& input : instruction.phiInputs)
                        if (edges.contains({input.predecessor, block.id}))
                            next = meet(next, values[input.value]);
                } else {
                    next = evaluate(instruction, values);
                }
                changed = update(values[*instruction.result], next) || changed;
            }
            const auto addEdge = [&](BlockId target) {
                if (edges.insert({block.id, target}).second) changed = true;
                if (!executable[target]) { executable[target] = true; changed = true; }
            };
            if (const auto* jump = std::get_if<IRJump>(&*block.terminator)) {
                addEdge(jump->target);
            } else if (const auto* branch = std::get_if<BranchValue>(&*block.terminator)) {
                const auto condition = values[branch->condition];
                if (condition.kind == LatticeKind::Constant)
                    addEdge(condition.constant != 0 ? branch->trueTarget : branch->falseTarget);
                else if (condition.kind == LatticeKind::Overdefined) {
                    addEdge(branch->trueTarget);
                    addEdge(branch->falseTarget);
                }
            }
        }
    }

    PassResult result;
    for (auto& block : function.blocks) {
        if (!block.terminator) continue;
        if (auto* branch = std::get_if<BranchValue>(&*block.terminator)) {
            const auto condition = values[branch->condition];
            if (condition.kind == LatticeKind::Constant) {
                const BlockId target = condition.constant != 0
                    ? branch->trueTarget : branch->falseTarget;
                block.terminator = IRJump{target};
                result.changed = true;
            }
        }
    }
    for (ValueId value = 0; value < values.size(); ++value) {
        if (values[value].kind != LatticeKind::Constant) continue;
        const IRInstruction* definition = findDefinition(function, value);
        if (definition && definition->op == IROp::Constant &&
            definition->immediate == values[value].constant)
            continue;
        const ValueId constant = getOrCreateEntryConstant(function, values[value].constant);
        if (replaceAllUses(function, value, constant)) {
            result.changed = true;
            ++result.instructionsReplaced;
        }
    }
    const auto oldBlocks = function.blocks.size();
    if (removeUnreachableIR(function)) result.changed = true;
    result.blocksRemoved += oldBlocks - function.blocks.size();
    if (eliminateTrivialPhis(function)) result.changed = true;
    compactIR(function);
    return result;
}

} // namespace toyc
