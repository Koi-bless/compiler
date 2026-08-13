#include "toyc/opt/inline.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <vector>

#include "toyc/opt/ir_utils.hpp"

namespace toyc {
namespace {

const IRFunction* findFunction(const IRModule& module, FuncId id) {
    for (const auto& function : module.functions)
        if (function.function == id) return &function;
    return nullptr;
}

bool reachesFunction(const IRModule& module, FuncId from, FuncId target,
                     std::vector<bool>& visited) {
    if (from >= visited.size() || visited[from]) return false;
    visited[from] = true;
    const auto* function = findFunction(module, from);
    if (!function) return false;
    for (const auto& block : function->blocks)
        for (const auto& instruction : block.instructions)
            if (instruction.op == IROp::Call && instruction.callee) {
                if (*instruction.callee == target) return true;
                if (reachesFunction(module, *instruction.callee, target, visited))
                    return true;
            }
    return false;
}

const IRFunction* inlineCandidate(const IRModule& module, FuncId caller,
                                  FuncId callee, std::size_t limit) {
    if (caller == callee) return nullptr;
    const IRFunction* function = findFunction(module, callee);
    if (!function || function->blocks.empty()) return nullptr;

    std::size_t instructionCount = 0;
    std::size_t returns = 0;
    for (const auto& block : function->blocks) {
        instructionCount += static_cast<std::size_t>(std::count_if(
            block.instructions.begin(), block.instructions.end(),
            [](const IRInstruction& instruction) {
                return instruction.op != IROp::Param;
            }));
        if (std::holds_alternative<ReturnValue>(*block.terminator)) ++returns;
        if (std::holds_alternative<IRUnreachable>(*block.terminator)) return nullptr;
    }
    if (instructionCount > limit || returns == 0) return nullptr;

    // Inlining an edge within a recursive call-graph SCC can grow forever and
    // also obscures the conservative termination model.
    std::vector<bool> visited(module.functions.size(), false);
    if (reachesFunction(module, callee, callee, visited)) return nullptr;
    std::fill(visited.begin(), visited.end(), false);
    if (reachesFunction(module, callee, caller, visited)) return nullptr;
    return function;
}

bool remapValue(ValueId& value, const std::map<ValueId, ValueId>& values) {
    const auto found = values.find(value);
    if (found == values.end()) return false;
    value = found->second;
    return true;
}

bool inlineOne(IRModule& module, IRFunction& caller, std::size_t& budget,
               std::size_t limit, PassResult& result) {
    for (BlockId callerBlock = 0; callerBlock < caller.blocks.size(); ++callerBlock) {
        for (std::size_t callIndex = 0;
             callIndex < caller.blocks[callerBlock].instructions.size(); ++callIndex) {
            const IRInstruction call =
                caller.blocks[callerBlock].instructions[callIndex];
            if (call.op != IROp::Call || !call.callee) continue;
            const IRFunction* callee = inlineCandidate(
                module, caller.function, *call.callee, limit);
            if (!callee) continue;

            std::size_t clonedCount = 0;
            for (const auto& block : callee->blocks)
                for (const auto& instruction : block.instructions)
                    if (instruction.op != IROp::Param) ++clonedCount;
            if (clonedCount > budget) continue;

            const BlockId cloneBase = static_cast<BlockId>(caller.blocks.size());
            const BlockId continuationId = static_cast<BlockId>(
                caller.blocks.size() + callee->blocks.size());
            std::vector<BlockId> blockMap(callee->blocks.size());
            for (BlockId id = 0; id < callee->blocks.size(); ++id)
                blockMap[id] = cloneBase + id;

            std::map<ValueId, ValueId> values;
            bool valid = true;
            for (const auto& block : callee->blocks)
                for (const auto& instruction : block.instructions) {
                    if (instruction.op == IROp::Param) {
                        if (!instruction.immediate || !instruction.result ||
                            *instruction.immediate < 0 ||
                            static_cast<std::size_t>(*instruction.immediate) >=
                                call.operands.size()) {
                            valid = false;
                            break;
                        }
                        values.emplace(*instruction.result,
                            call.operands[static_cast<std::size_t>(
                                *instruction.immediate)]);
                    } else if (instruction.result) {
                        values.emplace(*instruction.result, caller.valueCount++);
                    }
                }
            if (!valid) continue;

            std::vector<IRBlock> clones;
            clones.reserve(callee->blocks.size());
            std::vector<PhiInput> returnInputs;
            for (const auto& source : callee->blocks) {
                IRBlock clone;
                clone.id = blockMap[source.id];
                for (const auto& instruction : source.instructions) {
                    if (instruction.op == IROp::Param) continue;
                    IRInstruction copied = instruction;
                    copied.id = caller.instructionCount++;
                    if (copied.result) *copied.result = values.at(*copied.result);
                    for (auto& operand : copied.operands)
                        valid = remapValue(operand, values) && valid;
                    for (auto& input : copied.phiInputs) {
                        valid = remapValue(input.value, values) && valid;
                        if (input.predecessor >= blockMap.size()) valid = false;
                        else input.predecessor = blockMap[input.predecessor];
                    }
                    clone.instructions.push_back(std::move(copied));
                }
                if (const auto* jump = std::get_if<IRJump>(&*source.terminator)) {
                    if (jump->target >= blockMap.size()) valid = false;
                    else clone.terminator = IRJump{blockMap[jump->target]};
                } else if (const auto* branch =
                               std::get_if<BranchValue>(&*source.terminator)) {
                    if (branch->trueTarget >= blockMap.size() ||
                        branch->falseTarget >= blockMap.size()) {
                        valid = false;
                    } else {
                        ValueId condition = branch->condition;
                        valid = remapValue(condition, values) && valid;
                        clone.terminator = BranchValue{
                            condition, blockMap[branch->trueTarget],
                            blockMap[branch->falseTarget]};
                    }
                } else if (const auto* returned =
                               std::get_if<ReturnValue>(&*source.terminator)) {
                    if (returned->value) {
                        ValueId value = *returned->value;
                        valid = remapValue(value, values) && valid;
                        returnInputs.push_back({clone.id, value});
                    } else if (call.result) {
                        valid = false;
                    }
                    clone.terminator = IRJump{continuationId};
                } else {
                    valid = false;
                }
                clones.push_back(std::move(clone));
            }
            if (!valid || (call.result && returnInputs.empty())) continue;

            IRBlock continuation;
            continuation.id = continuationId;
            auto& original = caller.blocks[callerBlock];
            continuation.instructions.assign(
                std::make_move_iterator(original.instructions.begin() +
                    static_cast<std::ptrdiff_t>(callIndex + 1)),
                std::make_move_iterator(original.instructions.end()));
            continuation.terminator = std::move(original.terminator);

            std::optional<ValueId> replacement;
            if (call.result) {
                if (returnInputs.size() == 1) {
                    replacement = returnInputs.front().value;
                } else {
                    IRInstruction phi;
                    phi.id = caller.instructionCount++;
                    phi.op = IROp::Phi;
                    phi.result = caller.valueCount++;
                    phi.phiInputs = returnInputs;
                    std::sort(phi.phiInputs.begin(), phi.phiInputs.end(),
                              [](const PhiInput& lhs, const PhiInput& rhs) {
                                  return lhs.predecessor < rhs.predecessor;
                              });
                    replacement = phi.result;
                    continuation.instructions.insert(
                        continuation.instructions.begin(), std::move(phi));
                }
            }

            // Successor phis previously named the call block; the original
            // terminator now executes in the continuation block.
            for (auto& block : caller.blocks)
                for (auto& instruction : block.instructions) {
                    if (instruction.op != IROp::Phi) break;
                    for (auto& input : instruction.phiInputs)
                        if (input.predecessor == callerBlock)
                            input.predecessor = continuationId;
                }

            original.instructions.erase(
                original.instructions.begin() +
                    static_cast<std::ptrdiff_t>(callIndex),
                original.instructions.end());
            original.terminator = IRJump{blockMap[callee->entry]};
            caller.blocks.insert(caller.blocks.end(),
                                 std::make_move_iterator(clones.begin()),
                                 std::make_move_iterator(clones.end()));
            caller.blocks.push_back(std::move(continuation));
            if (call.result) replaceAllUses(caller, *call.result, *replacement);

            budget -= clonedCount;
            result.changed = true;
            ++result.instructionsRemoved;
            result.instructionsReplaced += clonedCount;
            canonicalizeIR(caller);
            return true;
        }
    }
    return false;
}

} // namespace

PassResult runFunctionInlining(IRModule& module, std::size_t growthBudget,
                               std::size_t calleeInstructionLimit) {
    PassResult result;
    bool changed = true;
    while (changed && growthBudget != 0) {
        changed = false;
        for (auto& function : module.functions) {
            if (inlineOne(module, function, growthBudget, calleeInstructionLimit,
                          result)) {
                changed = true;
                break;
            }
        }
    }
    return result;
}

} // namespace toyc
