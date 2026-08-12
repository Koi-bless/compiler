#include "toyc/opt/ir_utils.hpp"

#include <algorithm>
#include <bit>
#include <functional>
#include <limits>
#include <queue>
#include <unordered_map>

#include "toyc/support/diagnostic.hpp"

namespace toyc {
namespace {

[[noreturn]] void invalid(const IRFunction& function, const std::string& message) {
    throw CompileError(function.location, "IR transformation", message);
}

std::vector<BlockId> targets(const IRBlock& block) {
    std::vector<BlockId> result;
    if (!block.terminator) return result;
    if (const auto* jump = std::get_if<IRJump>(&*block.terminator)) {
        result.push_back(jump->target);
    } else if (const auto* branch = std::get_if<BranchValue>(&*block.terminator)) {
        result.push_back(branch->trueTarget);
        if (branch->falseTarget != branch->trueTarget)
            result.push_back(branch->falseTarget);
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::size_t instructionTotal(const IRFunction& function) {
    std::size_t result = 0;
    for (const auto& block : function.blocks) result += block.instructions.size();
    return result;
}

} // namespace

IRUseLists buildUseLists(const IRFunction& function) {
    IRUseLists uses(function.valueCount);
    const auto record = [&](ValueId value, IRUse use) {
        if (value >= uses.size()) invalid(function, "use references an out-of-range value");
        uses[value].push_back(use);
    };
    for (const auto& block : function.blocks) {
        for (const auto& instruction : block.instructions) {
            for (std::size_t index = 0; index < instruction.operands.size(); ++index)
                record(instruction.operands[index], {block.id, instruction.id, index, false, false});
            for (std::size_t index = 0; index < instruction.phiInputs.size(); ++index)
                record(instruction.phiInputs[index].value,
                       {block.id, instruction.id, index, true, false});
        }
        if (!block.terminator) continue;
        if (const auto* branch = std::get_if<BranchValue>(&*block.terminator))
            record(branch->condition, {block.id, std::nullopt, 0, false, true});
        else if (const auto* returned = std::get_if<ReturnValue>(&*block.terminator);
                 returned && returned->value)
            record(*returned->value, {block.id, std::nullopt, 0, false, true});
    }
    return uses;
}

std::vector<bool> computeReachable(const IRFunction& function) {
    std::vector<bool> reachable(function.blocks.size(), false);
    if (function.entry >= function.blocks.size()) return reachable;
    std::queue<BlockId> work;
    work.push(function.entry);
    reachable[function.entry] = true;
    while (!work.empty()) {
        const BlockId block = work.front();
        work.pop();
        for (const BlockId successor : targets(function.blocks[block])) {
            if (successor >= function.blocks.size())
                invalid(function, "terminator target is outside the function");
            if (!reachable[successor]) {
                reachable[successor] = true;
                work.push(successor);
            }
        }
    }
    return reachable;
}

std::vector<BlockId> computeReversePostOrder(const IRFunction& function) {
    std::vector<BlockId> postorder;
    std::vector<bool> visited(function.blocks.size(), false);
    std::function<void(BlockId)> visit = [&](BlockId block) {
        visited[block] = true;
        for (const BlockId successor : targets(function.blocks[block])) {
            if (successor >= function.blocks.size())
                invalid(function, "terminator target is outside the function");
            if (!visited[successor]) visit(successor);
        }
        postorder.push_back(block);
    };
    if (function.entry < function.blocks.size()) visit(function.entry);
    std::reverse(postorder.begin(), postorder.end());
    return postorder;
}

bool replaceAllUses(IRFunction& function, ValueId from, ValueId to) {
    if (from == to) return false;
    bool changed = false;
    for (auto& block : function.blocks) {
        for (auto& instruction : block.instructions) {
            for (auto& operand : instruction.operands) if (operand == from) {
                operand = to;
                changed = true;
            }
            for (auto& input : instruction.phiInputs) if (input.value == from) {
                input.value = to;
                changed = true;
            }
        }
        if (!block.terminator) continue;
        if (auto* branch = std::get_if<BranchValue>(&*block.terminator);
            branch && branch->condition == from) {
            branch->condition = to;
            changed = true;
        } else if (auto* returned = std::get_if<ReturnValue>(&*block.terminator);
                   returned && returned->value == from) {
            returned->value = to;
            changed = true;
        }
    }
    return changed;
}

void rebuildIRControlFlow(IRFunction& function) {
    for (auto& block : function.blocks) {
        if (!block.terminator) invalid(function, "block has no terminator");
        block.successors = targets(block);
        for (const BlockId successor : block.successors)
            if (successor >= function.blocks.size())
                invalid(function, "terminator target is outside the function");
        block.predecessors.clear();
    }
    for (const auto& block : function.blocks)
        for (const BlockId successor : block.successors)
            function.blocks[successor].predecessors.push_back(block.id);
    for (auto& block : function.blocks) {
        std::sort(block.predecessors.begin(), block.predecessors.end());
        block.predecessors.erase(
            std::unique(block.predecessors.begin(), block.predecessors.end()),
            block.predecessors.end());
        for (auto& instruction : block.instructions) {
            if (instruction.op != IROp::Phi) break;
            instruction.phiInputs.erase(
                std::remove_if(instruction.phiInputs.begin(), instruction.phiInputs.end(),
                               [&](const PhiInput& input) {
                                   return !std::binary_search(block.predecessors.begin(),
                                                              block.predecessors.end(),
                                                              input.predecessor);
                               }),
                instruction.phiInputs.end());
            std::sort(instruction.phiInputs.begin(), instruction.phiInputs.end(),
                      [](const PhiInput& lhs, const PhiInput& rhs) {
                          return lhs.predecessor < rhs.predecessor;
                      });
            if (instruction.phiInputs.size() != block.predecessors.size())
                invalid(function, "new CFG edge lacks a phi input");
            for (std::size_t index = 0; index < block.predecessors.size(); ++index)
                if (instruction.phiInputs[index].predecessor != block.predecessors[index])
                    invalid(function, "phi inputs do not match predecessors");
        }
    }
}

bool removeUnreachableIR(IRFunction& function) {
    rebuildIRControlFlow(function);
    const auto reachable = computeReachable(function);
    if (std::all_of(reachable.begin(), reachable.end(), [](bool value) { return value; }))
        return false;
    const BlockId invalidId = std::numeric_limits<BlockId>::max();
    std::vector<BlockId> mapping(function.blocks.size(), invalidId);
    std::vector<IRBlock> blocks;
    blocks.reserve(function.blocks.size());
    for (const auto& block : function.blocks) if (reachable[block.id]) {
        mapping[block.id] = static_cast<BlockId>(blocks.size());
        blocks.push_back(block);
        blocks.back().id = static_cast<BlockId>(blocks.size() - 1);
    }
    for (auto& block : blocks) {
        for (auto& instruction : block.instructions) if (instruction.op == IROp::Phi) {
            instruction.phiInputs.erase(
                std::remove_if(instruction.phiInputs.begin(), instruction.phiInputs.end(),
                               [&](const PhiInput& input) {
                                   return input.predecessor >= reachable.size() ||
                                          !reachable[input.predecessor];
                               }),
                instruction.phiInputs.end());
            for (auto& input : instruction.phiInputs)
                input.predecessor = mapping[input.predecessor];
        }
        if (auto* jump = std::get_if<IRJump>(&*block.terminator))
            jump->target = mapping[jump->target];
        else if (auto* branch = std::get_if<BranchValue>(&*block.terminator)) {
            branch->trueTarget = mapping[branch->trueTarget];
            branch->falseTarget = mapping[branch->falseTarget];
        }
    }
    function.entry = mapping[function.entry];
    function.blocks = std::move(blocks);
    rebuildIRControlFlow(function);
    compactIR(function);
    return true;
}

bool eliminateTrivialPhis(IRFunction& function) {
    bool anyChanged = false;
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& block : function.blocks) {
            for (auto iterator = block.instructions.begin(); iterator != block.instructions.end(); ++iterator) {
                if (iterator->op != IROp::Phi) break;
                const ValueId result = *iterator->result;
                std::optional<ValueId> replacement;
                bool nonTrivial = false;
                for (const auto& input : iterator->phiInputs) {
                    if (input.value == result) continue;
                    if (!replacement) replacement = input.value;
                    else if (*replacement != input.value) { nonTrivial = true; break; }
                }
                if (nonTrivial || !replacement) continue;
                replaceAllUses(function, result, *replacement);
                block.instructions.erase(iterator);
                changed = true;
                anyChanged = true;
                break;
            }
            if (changed) break;
        }
    }
    if (anyChanged) compactIR(function);
    return anyChanged;
}

void compactIR(IRFunction& function) {
    const ValueId invalidValue = std::numeric_limits<ValueId>::max();
    std::vector<ValueId> mapping(function.valueCount, invalidValue);
    ValueId nextValue = 0;
    InstId nextInstruction = 0;
    for (auto& block : function.blocks) {
        for (auto& instruction : block.instructions) {
            instruction.id = nextInstruction++;
            if (instruction.result) {
                if (*instruction.result >= mapping.size())
                    mapping.resize(static_cast<std::size_t>(*instruction.result) + 1, invalidValue);
                if (mapping[*instruction.result] != invalidValue)
                    invalid(function, "value has multiple definitions while compacting");
                mapping[*instruction.result] = nextValue++;
            }
        }
    }
    const auto remap = [&](ValueId& value) {
        if (value >= mapping.size() || mapping[value] == invalidValue)
            invalid(function, "use has no surviving definition while compacting");
        value = mapping[value];
    };
    for (auto& block : function.blocks) {
        for (auto& instruction : block.instructions) {
            if (instruction.result) remap(*instruction.result);
            for (auto& operand : instruction.operands) remap(operand);
            for (auto& input : instruction.phiInputs) remap(input.value);
        }
        if (auto* branch = std::get_if<BranchValue>(&*block.terminator))
            remap(branch->condition);
        else if (auto* returned = std::get_if<ReturnValue>(&*block.terminator);
                 returned && returned->value)
            remap(*returned->value);
    }
    function.valueCount = nextValue;
    function.instructionCount = nextInstruction;
}

bool canonicalizeIR(IRFunction& function) {
    const auto oldBlocks = function.blocks.size();
    const auto oldInstructions = instructionTotal(function);
    rebuildIRControlFlow(function);
    const bool unreachable = removeUnreachableIR(function);
    const bool phis = eliminateTrivialPhis(function);
    compactIR(function);
    return unreachable || phis || oldBlocks != function.blocks.size() ||
           oldInstructions != instructionTotal(function);
}

bool canonicalizeIR(IRModule& module) {
    bool changed = false;
    for (auto& function : module.functions)
        changed = canonicalizeIR(function) || changed;
    return changed;
}

IRInstruction* findDefinition(IRFunction& function, ValueId value) {
    for (auto& block : function.blocks)
        for (auto& instruction : block.instructions)
            if (instruction.result == value) return &instruction;
    return nullptr;
}

const IRInstruction* findDefinition(const IRFunction& function, ValueId value) {
    for (const auto& block : function.blocks)
        for (const auto& instruction : block.instructions)
            if (instruction.result == value) return &instruction;
    return nullptr;
}

ValueId getOrCreateEntryConstant(IRFunction& function, std::int32_t value) {
    if (function.entry >= function.blocks.size()) invalid(function, "invalid entry block");
    auto& instructions = function.blocks[function.entry].instructions;
    for (auto iterator = instructions.begin(); iterator != instructions.end(); ++iterator)
        if (iterator->op == IROp::Constant && iterator->immediate == value) {
            const ValueId result = *iterator->result;
            if (iterator != instructions.begin()) {
                IRInstruction constant = std::move(*iterator);
                instructions.erase(iterator);
                instructions.insert(instructions.begin(), std::move(constant));
            }
            return result;
        }
    IRInstruction constant;
    constant.id = function.instructionCount++;
    constant.op = IROp::Constant;
    constant.result = function.valueCount++;
    constant.immediate = value;
    instructions.insert(instructions.begin(), constant);
    return *constant.result;
}

std::int32_t wrapAdd(std::int32_t lhs, std::int32_t rhs) {
    return std::bit_cast<std::int32_t>(std::bit_cast<std::uint32_t>(lhs) +
                                       std::bit_cast<std::uint32_t>(rhs));
}

std::int32_t wrapSub(std::int32_t lhs, std::int32_t rhs) {
    return std::bit_cast<std::int32_t>(std::bit_cast<std::uint32_t>(lhs) -
                                       std::bit_cast<std::uint32_t>(rhs));
}

std::int32_t wrapMul(std::int32_t lhs, std::int32_t rhs) {
    return std::bit_cast<std::int32_t>(std::bit_cast<std::uint32_t>(lhs) *
                                       std::bit_cast<std::uint32_t>(rhs));
}

std::optional<std::int32_t> foldUnary(IROp op, std::int32_t operand) {
    if (op == IROp::LogicalNot) return operand == 0 ? 1 : 0;
    return std::nullopt;
}

std::optional<std::int32_t> foldBinary(IROp op, std::int32_t lhs,
                                       std::int32_t rhs) {
    switch (op) {
    case IROp::Add: return wrapAdd(lhs, rhs);
    case IROp::Sub: return wrapSub(lhs, rhs);
    case IROp::Mul: return wrapMul(lhs, rhs);
    case IROp::SDiv:
        if (rhs == 0 || (lhs == std::numeric_limits<std::int32_t>::min() && rhs == -1))
            return std::nullopt;
        return lhs / rhs;
    case IROp::SRem:
        if (rhs == 0) return std::nullopt;
        if (lhs == std::numeric_limits<std::int32_t>::min() && rhs == -1) return 0;
        return lhs % rhs;
    case IROp::ICmpLT: return lhs < rhs ? 1 : 0;
    case IROp::ICmpGT: return lhs > rhs ? 1 : 0;
    case IROp::ICmpLE: return lhs <= rhs ? 1 : 0;
    case IROp::ICmpGE: return lhs >= rhs ? 1 : 0;
    case IROp::ICmpEQ: return lhs == rhs ? 1 : 0;
    case IROp::ICmpNE: return lhs != rhs ? 1 : 0;
    default: return std::nullopt;
    }
}

bool producesValue(IROp op, const IRInstruction& instruction,
                   const SemanticResult& semantic) {
    if (op == IROp::StoreGlobal) return false;
    if (op == IROp::Call && instruction.callee &&
        *instruction.callee < semantic.functions.size())
        return semantic.functions[*instruction.callee].returnType != ValueType::Void;
    return true;
}

bool hasSideEffects(const IRInstruction& instruction) {
    return instruction.op == IROp::StoreGlobal || instruction.op == IROp::Call;
}

bool readsMemory(const IRInstruction& instruction) {
    return instruction.op == IROp::LoadGlobal || instruction.op == IROp::Call;
}

bool mayTrap(const IRInstruction& instruction) {
    return instruction.op == IROp::SDiv || instruction.op == IROp::SRem;
}

bool isKnownNonTrapping(const IRInstruction& instruction,
                        const IRFunction& function) {
    if (!mayTrap(instruction)) return true;
    if (instruction.operands.size() != 2) return false;
    const auto* divisor = findDefinition(function, instruction.operands[1]);
    if (!divisor || divisor->op != IROp::Constant || !divisor->immediate ||
        *divisor->immediate == 0)
        return false;
    if (instruction.op == IROp::SRem) return true;
    if (*divisor->immediate != -1) return true;
    const auto* dividend = findDefinition(function, instruction.operands[0]);
    return dividend && dividend->op == IROp::Constant && dividend->immediate &&
           *dividend->immediate != std::numeric_limits<std::int32_t>::min();
}

bool isPure(const IRInstruction& instruction) {
    return instruction.op != IROp::Phi && instruction.op != IROp::Param &&
           !readsMemory(instruction) && !hasSideEffects(instruction);
}

bool isSafeToSpeculate(const IRInstruction& instruction) {
    return isPure(instruction) && !mayTrap(instruction);
}

bool isCommutative(IROp op) {
    return op == IROp::Add || op == IROp::Mul || op == IROp::ICmpEQ ||
           op == IROp::ICmpNE;
}

} // namespace toyc
