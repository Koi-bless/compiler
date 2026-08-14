#include "toyc/opt/global_localize.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <queue>
#include <set>
#include <vector>

#include "toyc/ir/dominator.hpp"
#include "toyc/opt/ir_utils.hpp"
#include "toyc/opt/loop_analysis.hpp"

namespace toyc {
namespace {

struct Candidate {
    bool written = false;
    bool usedInLoop = false;
    SourceLocation location{};
    std::set<BlockId> definitions;
};

bool callTouchesGlobal(const IRInstruction& call, SymbolId global,
                       const FunctionEffectAnalysis& effects) {
    if (call.op != IROp::Call) return false;
    if (!call.callee) return true;
    const auto* summary = effects.lookup(*call.callee);
    return !summary || summary->reads.contains(global) ||
        summary->writes.contains(global);
}

} // namespace

PassResult runGlobalScalarLocalization(
    IRFunction& function, const FunctionEffectAnalysis& effects) {
    std::set<BlockId> loopBlocks;
    for (const auto& loop : analyzeLoops(function))
        loopBlocks.insert(loop.blocks.begin(), loop.blocks.end());
    if (loopBlocks.empty()) return {};

    std::map<SymbolId, Candidate> candidates;
    std::vector<const IRInstruction*> calls;
    for (const auto& block : function.blocks) {
        for (const auto& instruction : block.instructions) {
            if (instruction.op == IROp::Call) calls.push_back(&instruction);
            if ((instruction.op != IROp::LoadGlobal &&
                 instruction.op != IROp::StoreGlobal) ||
                !instruction.global)
                continue;
            auto& candidate = candidates[*instruction.global];
            candidate.location = instruction.location;
            candidate.usedInLoop = candidate.usedInLoop ||
                loopBlocks.contains(block.id);
            if (instruction.op == IROp::StoreGlobal) {
                candidate.written = true;
                candidate.definitions.insert(block.id);
            }
        }
    }
    for (auto iterator = candidates.begin(); iterator != candidates.end();) {
        const bool blocked = !iterator->second.usedInLoop || std::any_of(
            calls.begin(), calls.end(), [&](const IRInstruction* call) {
                return callTouchesGlobal(*call, iterator->first, effects);
            });
        if (blocked)
            iterator = candidates.erase(iterator);
        else
            ++iterator;
    }
    if (candidates.empty()) return {};

    DominatorInfo dominators(function);
    std::map<BlockId, std::map<SymbolId, ValueId>> phiValues;
    for (const auto& [global, candidate] : candidates) {
        std::queue<BlockId> work;
        std::set<BlockId> queued = candidate.definitions;
        std::set<BlockId> placed;
        for (const BlockId block : candidate.definitions) work.push(block);
        while (!work.empty()) {
            const BlockId block = work.front();
            work.pop();
            for (const BlockId frontier : dominators.frontier(block)) {
                if (!placed.insert(frontier).second) continue;
                phiValues[frontier].emplace(global, function.valueCount++);
                if (queued.insert(frontier).second) work.push(frontier);
            }
        }
    }

    std::map<SymbolId, ValueId> initialValues;
    auto& entryInstructions = function.blocks[function.entry].instructions;
    auto entryInsertion = std::find_if(
        entryInstructions.begin(), entryInstructions.end(),
        [](const IRInstruction& instruction) {
            return instruction.op != IROp::Param;
        });
    for (const auto& [global, candidate] : candidates) {
        IRInstruction load;
        load.id = function.instructionCount++;
        load.op = IROp::LoadGlobal;
        load.result = function.valueCount++;
        load.global = global;
        load.location = candidate.location;
        initialValues.emplace(global, *load.result);
        entryInsertion = entryInstructions.insert(entryInsertion, std::move(load));
        ++entryInsertion;
    }

    for (auto& [blockId, globals] : phiValues) {
        auto& instructions = function.blocks[blockId].instructions;
        auto insertion = std::find_if(
            instructions.begin(), instructions.end(),
            [](const IRInstruction& instruction) {
                return instruction.op != IROp::Phi;
            });
        for (const auto& [global, value] : globals) {
            IRInstruction phi;
            phi.id = function.instructionCount++;
            phi.op = IROp::Phi;
            phi.result = value;
            for (const BlockId predecessor :
                 function.blocks[blockId].predecessors)
                phi.phiInputs.push_back(
                    {predecessor, initialValues.at(global)});
            phi.location = candidates.at(global).location;
            insertion = instructions.insert(insertion, std::move(phi));
            ++insertion;
        }
    }

    std::map<SymbolId, std::vector<ValueId>> states;
    for (const auto& [global, initial] : initialValues)
        states[global].push_back(initial);

    PassResult result;
    const auto rename = [&](const auto& self, BlockId blockId) -> void {
        std::map<SymbolId, std::size_t> pushed;
        if (const auto found = phiValues.find(blockId);
            found != phiValues.end()) {
            for (const auto& [global, value] : found->second) {
                states[global].push_back(value);
                ++pushed[global];
            }
        }

        auto& instructions = function.blocks[blockId].instructions;
        for (auto& instruction : instructions) {
            if (!instruction.global ||
                !candidates.contains(*instruction.global))
                continue;
            const SymbolId global = *instruction.global;
            if (instruction.op == IROp::LoadGlobal && instruction.result &&
                *instruction.result != initialValues.at(global)) {
                replaceAllUses(function, *instruction.result,
                               states.at(global).back());
            } else if (instruction.op == IROp::StoreGlobal) {
                states[global].push_back(instruction.operands.front());
                ++pushed[global];
            }
        }
        const auto oldSize = instructions.size();
        instructions.erase(std::remove_if(
            instructions.begin(), instructions.end(),
            [&](const IRInstruction& instruction) {
                if (!instruction.global ||
                    !candidates.contains(*instruction.global))
                    return false;
                const SymbolId global = *instruction.global;
                if (instruction.op == IROp::StoreGlobal) return true;
                return instruction.op == IROp::LoadGlobal &&
                    instruction.result != initialValues.at(global);
            }), instructions.end());
        result.instructionsRemoved += oldSize - instructions.size();

        for (const BlockId successor :
             function.blocks[blockId].successors) {
            const auto phis = phiValues.find(successor);
            if (phis == phiValues.end()) continue;
            for (const auto& [global, value] : phis->second) {
                auto* phi = findDefinition(function, value);
                auto input = std::find_if(
                    phi->phiInputs.begin(), phi->phiInputs.end(),
                    [&](const PhiInput& candidate) {
                        return candidate.predecessor == blockId;
                    });
                input->value = states.at(global).back();
            }
        }

        if (std::holds_alternative<ReturnValue>(
                *function.blocks[blockId].terminator)) {
            for (const auto& [global, candidate] : candidates) {
                if (!candidate.written) continue;
                IRInstruction store;
                store.id = function.instructionCount++;
                store.op = IROp::StoreGlobal;
                store.operands = {states.at(global).back()};
                store.global = global;
                store.location = candidate.location;
                instructions.push_back(std::move(store));
            }
        }

        for (const BlockId child : dominators.children(blockId))
            self(self, child);
        for (const auto& [global, count] : pushed)
            for (std::size_t index = 0; index < count; ++index)
                states[global].pop_back();
    };
    rename(rename, function.entry);

    result.changed = true;
    result.instructionsReplaced += candidates.size();
    compactIR(function);
    return result;
}

} // namespace toyc
