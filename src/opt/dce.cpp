#include "toyc/opt/dce.hpp"

#include <algorithm>
#include <queue>
#include <vector>

#include "toyc/opt/ir_utils.hpp"

namespace toyc {

PassResult runDCE(IRFunction& function, bool preserveMayTrap) {
    std::vector<IRInstruction*> definitions(function.valueCount, nullptr);
    for (auto& block : function.blocks) for (auto& instruction : block.instructions) {
        if (instruction.result) definitions[*instruction.result] = &instruction;
    }
    std::vector<bool> live(function.instructionCount, false);
    std::queue<IRInstruction*> work;
    const auto markInstruction = [&](IRInstruction* instruction) {
        if (instruction && !live[instruction->id]) {
            live[instruction->id] = true;
            work.push(instruction);
        }
    };
    const auto markValue = [&](ValueId value) {
        if (value < definitions.size()) markInstruction(definitions[value]);
    };
    for (auto& block : function.blocks) {
        for (auto& instruction : block.instructions)
            if (hasSideEffects(instruction) ||
                (preserveMayTrap && mayTrap(instruction) &&
                 !isKnownNonTrapping(instruction, function)))
                markInstruction(&instruction);
        if (const auto* branch = std::get_if<BranchValue>(&*block.terminator))
            markValue(branch->condition);
        else if (const auto* returned = std::get_if<ReturnValue>(&*block.terminator);
                 returned && returned->value)
            markValue(*returned->value);
    }
    while (!work.empty()) {
        IRInstruction* instruction = work.front();
        work.pop();
        for (const ValueId operand : instruction->operands) markValue(operand);
        for (const auto& input : instruction->phiInputs) markValue(input.value);
    }

    PassResult result;
    for (auto& block : function.blocks) {
        const auto oldSize = block.instructions.size();
        block.instructions.erase(
            std::remove_if(block.instructions.begin(), block.instructions.end(),
                           [&](const IRInstruction& instruction) {
                               return !live[instruction.id];
                           }),
            block.instructions.end());
        result.instructionsRemoved += oldSize - block.instructions.size();
    }
    result.changed = result.instructionsRemoved != 0;
    if (result.changed) compactIR(function);
    return result;
}

} // namespace toyc
