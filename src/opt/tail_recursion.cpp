#include "toyc/opt/tail_recursion.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <vector>

#include "toyc/opt/ir_utils.hpp"

namespace toyc {
namespace {

struct TailSite {
    BlockId block{};
    std::size_t instruction{};
};

} // namespace

PassResult runTailRecursionElimination(IRFunction& function) {
    if (function.entry >= function.blocks.size()) return {};
    std::vector<TailSite> sites;
    for (const auto& block : function.blocks) {
        for (std::size_t index = 0; index < block.instructions.size(); ++index) {
            const auto& instruction = block.instructions[index];
            if (instruction.op != IROp::Call ||
                instruction.callee != function.function)
                continue;
            const auto* returned = std::get_if<ReturnValue>(&*block.terminator);
            const bool finalInstruction = index + 1 == block.instructions.size();
            const bool returnedDirectly = returned &&
                ((instruction.result && returned->value == instruction.result) ||
                 (!instruction.result && !returned->value));
            if (!finalInstruction || !returnedDirectly) return {};
            sites.push_back({block.id, index});
        }
    }
    if (sites.empty()) return {};
    if (std::any_of(sites.begin(), sites.end(), [&](const TailSite& site) {
            return site.block == function.entry;
        }))
        return {};

    auto& entry = function.blocks[function.entry];
    std::map<std::size_t, ValueId> parameters;
    for (const auto& instruction : entry.instructions) {
        if (instruction.op != IROp::Param || !instruction.result ||
            !instruction.immediate || *instruction.immediate < 0)
            continue;
        parameters.emplace(static_cast<std::size_t>(*instruction.immediate),
                           *instruction.result);
    }
    if (parameters.empty()) return {};
    for (std::size_t index = 0; index < parameters.size(); ++index)
        if (!parameters.contains(index)) return {};
    for (const auto& site : sites)
        if (function.blocks[site.block].instructions[site.instruction].operands.size() !=
            parameters.size()) return {};

    const BlockId oldEntry = function.entry;
    const BlockId headerId = static_cast<BlockId>(function.blocks.size());
    std::vector<ValueId> phiValues(parameters.size());
    for (std::size_t index = 0; index < parameters.size(); ++index)
        phiValues[index] = function.valueCount++;

    // Rewrite the old body before creating phi inputs.  This gives recursive
    // arguments parallel-copy semantics: every argument expression observes
    // the old iteration's parameter phis.
    for (std::size_t index = 0; index < parameters.size(); ++index)
        replaceAllUses(function, parameters[index], phiValues[index]);

    std::vector<std::vector<ValueId>> backedgeArguments;
    backedgeArguments.reserve(sites.size());
    for (const auto& site : sites)
        backedgeArguments.push_back(
            function.blocks[site.block].instructions[site.instruction].operands);

    IRBlock header;
    header.id = headerId;
    for (std::size_t index = 0; index < parameters.size(); ++index) {
        IRInstruction phi;
        phi.id = function.instructionCount++;
        phi.op = IROp::Phi;
        phi.result = phiValues[index];
        phi.phiInputs.push_back({oldEntry, parameters[index]});
        for (std::size_t site = 0; site < sites.size(); ++site)
            phi.phiInputs.push_back({sites[site].block, backedgeArguments[site][index]});
        std::sort(phi.phiInputs.begin(), phi.phiInputs.end(),
            [](const PhiInput& lhs, const PhiInput& rhs) {
                return lhs.predecessor < rhs.predecessor;
            });
        header.instructions.push_back(std::move(phi));
    }

    for (auto iterator = entry.instructions.begin(); iterator != entry.instructions.end();) {
        if (iterator->op == IROp::Param) {
            ++iterator;
        } else {
            header.instructions.push_back(std::move(*iterator));
            iterator = entry.instructions.erase(iterator);
        }
    }
    header.terminator = std::move(entry.terminator);
    entry.terminator = IRJump{headerId};

    // Edges formerly leaving the entry now leave the loop header.
    for (auto& block : function.blocks) {
        for (auto& instruction : block.instructions) {
            if (instruction.op != IROp::Phi) break;
            for (auto& input : instruction.phiInputs)
                if (input.predecessor == oldEntry) input.predecessor = headerId;
            std::sort(instruction.phiInputs.begin(), instruction.phiInputs.end(),
                [](const PhiInput& lhs, const PhiInput& rhs) {
                    return lhs.predecessor < rhs.predecessor;
                });
        }
    }

    function.blocks.push_back(std::move(header));
    for (const auto& site : sites) {
        auto& block = function.blocks[site.block];
        block.instructions.erase(block.instructions.begin() +
            static_cast<std::ptrdiff_t>(site.instruction));
        block.terminator = IRJump{headerId};
    }
    canonicalizeIR(function);
    PassResult result;
    result.changed = true;
    result.instructionsRemoved = sites.size();
    result.instructionsReplaced = parameters.size();
    return result;
}

} // namespace toyc
