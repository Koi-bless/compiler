#include "toyc/opt/inline.hpp"

#include <map>
#include <optional>
#include <vector>

#include "toyc/opt/ir_utils.hpp"

namespace toyc {
namespace {

const IRFunction* inlineCandidate(const IRModule& module, FuncId caller,
                                  FuncId callee, std::size_t limit) {
    if (caller == callee) return nullptr;
    const IRFunction* function = nullptr;
    for (const auto& candidate : module.functions)
        if (candidate.function == callee) function = &candidate;
    if (!function || function->blocks.size() != 1 ||
        function->blocks[0].instructions.size() > limit ||
        !std::holds_alternative<ReturnValue>(*function->blocks[0].terminator))
        return nullptr;
    for (const auto& instruction : function->blocks[0].instructions)
        if (instruction.op == IROp::Phi || instruction.op == IROp::Call ||
            instruction.op == IROp::StoreGlobal)
            return nullptr;
    return function;
}

bool inlineOne(IRModule& module, IRFunction& caller, std::size_t& budget,
               std::size_t limit, PassResult& result) {
    for (auto& block : caller.blocks) {
        for (std::size_t index = 0; index < block.instructions.size(); ++index) {
            const IRInstruction call = block.instructions[index];
            if (call.op != IROp::Call || !call.callee) continue;
            const IRFunction* callee = inlineCandidate(module, caller.function,
                                                       *call.callee, limit);
            if (!callee) continue;
            std::size_t clonedCount = 0;
            for (const auto& instruction : callee->blocks[0].instructions)
                if (instruction.op != IROp::Param) ++clonedCount;
            if (clonedCount > budget) continue;

            std::map<ValueId, ValueId> values;
            std::vector<IRInstruction> clones;
            clones.reserve(clonedCount);
            bool valid = true;
            for (const auto& instruction : callee->blocks[0].instructions) {
                if (instruction.op == IROp::Param) {
                    const auto parameter = static_cast<std::size_t>(*instruction.immediate);
                    if (parameter >= call.operands.size()) { valid = false; break; }
                    values.emplace(*instruction.result, call.operands[parameter]);
                    continue;
                }
                IRInstruction clone = instruction;
                clone.id = caller.instructionCount++;
                for (auto& operand : clone.operands) {
                    const auto found = values.find(operand);
                    if (found == values.end()) { valid = false; break; }
                    operand = found->second;
                }
                if (!valid) break;
                if (clone.result) {
                    const ValueId old = *clone.result;
                    clone.result = caller.valueCount++;
                    values.emplace(old, *clone.result);
                }
                clones.push_back(std::move(clone));
            }
            const auto* returned =
                std::get_if<ReturnValue>(&*callee->blocks[0].terminator);
            std::optional<ValueId> replacement;
            if (valid && returned && returned->value) {
                const auto found = values.find(*returned->value);
                if (found == values.end()) valid = false;
                else replacement = found->second;
            }
            if (!valid || call.result.has_value() != replacement.has_value()) continue;

            block.instructions.insert(
                block.instructions.begin() + static_cast<std::ptrdiff_t>(index),
                clones.begin(), clones.end());
            if (call.result) replaceAllUses(caller, *call.result, *replacement);
            block.instructions.erase(block.instructions.begin() +
                static_cast<std::ptrdiff_t>(index + clones.size()));
            budget -= clonedCount;
            result.changed = true;
            ++result.instructionsRemoved;
            result.instructionsReplaced += clonedCount;
            compactIR(caller);
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
