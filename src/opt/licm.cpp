#include "toyc/opt/licm.hpp"

#include <algorithm>
#include <set>
#include <vector>

#include "toyc/opt/ir_utils.hpp"
#include "toyc/opt/loop_analysis.hpp"

namespace toyc {
namespace {

bool allowed(IROp op) {
    switch (op) {
    case IROp::Constant: case IROp::Copy: case IROp::Add: case IROp::Sub:
    case IROp::Mul: case IROp::SDiv: case IROp::SRem:
    case IROp::ICmpLT: case IROp::ICmpGT: case IROp::ICmpLE:
    case IROp::ICmpGE: case IROp::ICmpEQ: case IROp::ICmpNE:
    case IROp::LogicalNot:
        return true;
    default:
        return false;
    }
}

} // namespace

PassResult runLICM(IRFunction& function) {
    auto loops = analyzeLoops(function);
    std::stable_sort(loops.begin(), loops.end(), [](const Loop& lhs, const Loop& rhs) {
        return lhs.depth > rhs.depth;
    });
    PassResult result;
    for (const auto& loop : loops) {
        if (!loop.preheader) continue;
        const std::set<BlockId> loopBlocks(loop.blocks.begin(), loop.blocks.end());
        // Hoisting never changes the CFG, so the reverse postorder and the
        // definition-block map are computed once; hoisting a value only moves
        // its definition to the preheader, which is tracked incrementally.
        const auto order = computeReversePostOrder(function);
        std::vector<BlockId> definitionBlock(function.valueCount, function.entry);
        for (const auto& block : function.blocks)
            for (const auto& instruction : block.instructions)
                if (instruction.result) definitionBlock[*instruction.result] = block.id;
        bool moved = true;
        while (moved) {
            moved = false;
            for (const BlockId blockId : order) {
                if (!loopBlocks.contains(blockId)) continue;
                auto& instructions = function.blocks[blockId].instructions;
                for (std::size_t index = 0; index < instructions.size();) {
                    const auto& instruction = instructions[index];
                    if (!instruction.result || !allowed(instruction.op) ||
                        (!isSafeToSpeculate(instruction) &&
                         !isKnownNonTrapping(instruction, function))) {
                        ++index;
                        continue;
                    }
                    const bool invariant = std::all_of(
                        instruction.operands.begin(), instruction.operands.end(),
                        [&](ValueId operand) {
                            return !loopBlocks.contains(definitionBlock[operand]);
                        });
                    if (!invariant) {
                        ++index;
                        continue;
                    }
                    const ValueId resultValue = *instruction.result;
                    IRInstruction hoisted = std::move(instructions[index]);
                    instructions.erase(instructions.begin() + static_cast<std::ptrdiff_t>(index));
                    function.blocks[*loop.preheader].instructions.push_back(std::move(hoisted));
                    definitionBlock[resultValue] = *loop.preheader;
                    result.changed = true;
                    ++result.instructionsReplaced;
                    moved = true;
                }
            }
        }
    }
    if (result.changed) compactIR(function);
    return result;
}

} // namespace toyc
