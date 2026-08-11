#include "toyc/backend/phi_lowering.hpp"

#include <algorithm>
#include <utility>

#include "toyc/ir/cfg_utils.hpp"
#include "toyc/support/diagnostic.hpp"

namespace toyc {

void splitCriticalEdges(IRModule& module) {
    for (auto& function : module.functions) {
        std::vector<std::pair<BlockId, BlockId>> edges;
        const std::size_t originalCount = function.blocks.size();
        for (std::size_t index = 0; index < originalCount; ++index) {
            const auto& block = function.blocks[index];
            if (block.successors.size() <= 1) continue;
            for (const BlockId successor : block.successors)
                if (function.blocks[successor].predecessors.size() > 1) edges.emplace_back(block.id, successor);
        }
        for (const auto& [from, to] : edges) splitEdge(function, from, to);
    }
}

void resolveParallelCopies(MachineFunction& function) {
    for (auto& block : function.blocks) {
        std::vector<MInstruction> output;
        for (auto& instruction : block.instructions) {
            if (instruction.opcode != MOpcode::PARALLEL_COPY) {
                output.push_back(std::move(instruction));
                continue;
            }
            if (instruction.defs.size() != instruction.uses.size())
                throw CompileError(function.location, "parallel copy", "definition/use count mismatch");
            std::vector<std::pair<MOperand, MOperand>> pending;
            for (std::size_t index = 0; index < instruction.defs.size(); ++index)
                if (instruction.defs[index] != instruction.uses[index])
                    pending.emplace_back(instruction.defs[index], instruction.uses[index]);
            while (!pending.empty()) {
                auto safe = pending.end();
                for (auto iterator = pending.begin(); iterator != pending.end(); ++iterator) {
                    const bool destinationNeeded = std::any_of(pending.begin(), pending.end(), [&](const auto& copy) {
                        return copy.second == iterator->first;
                    });
                    if (!destinationNeeded) { safe = iterator; break; }
                }
                if (safe != pending.end()) {
                    output.push_back(MInstruction{MOpcode::COPY, {safe->first}, {safe->second}, {}, {}, instruction.location});
                    pending.erase(safe);
                    continue;
                }
                const MOperand oldSource = pending.front().second;
                const MOperand temporary = VirtualReg{function.vregCount++};
                output.push_back(MInstruction{MOpcode::COPY, {temporary}, {oldSource}, {}, {}, instruction.location});
                for (auto& copy : pending) if (copy.second == oldSource) copy.second = temporary;
            }
        }
        block.instructions = std::move(output);
    }
}

void resolveParallelCopies(MachineModule& module) {
    for (auto& function : module.functions) resolveParallelCopies(function);
}

} // namespace toyc
