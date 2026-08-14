#include "toyc/opt/simplify_cfg.hpp"

#include <algorithm>
#include <optional>
#include <vector>

#include "toyc/opt/ir_utils.hpp"

namespace toyc {
namespace {

bool redirect(IRTerminator& terminator, BlockId from, BlockId to) {
    bool changed = false;
    if (auto* jump = std::get_if<IRJump>(&terminator)) {
        if (jump->target == from) { jump->target = to; changed = true; }
    } else if (auto* branch = std::get_if<BranchValue>(&terminator)) {
        if (branch->trueTarget == from) { branch->trueTarget = to; changed = true; }
        if (branch->falseTarget == from) { branch->falseTarget = to; changed = true; }
    }
    return changed;
}

std::optional<std::int32_t> constantValue(const IRFunction& function, ValueId value) {
    const auto* definition = findDefinition(function, value);
    if (definition && definition->op == IROp::Constant) return definition->immediate;
    return std::nullopt;
}

bool foldOneBranch(IRFunction& function) {
    for (auto& block : function.blocks) {
        auto* branch = std::get_if<BranchValue>(&*block.terminator);
        if (!branch) continue;
        std::optional<BlockId> target;
        if (branch->trueTarget == branch->falseTarget) target = branch->trueTarget;
        else if (const auto value = constantValue(function, branch->condition))
            target = *value != 0 ? branch->trueTarget : branch->falseTarget;
        if (target) {
            block.terminator = IRJump{*target};
            canonicalizeIR(function);
            return true;
        }
    }
    return false;
}

bool bypassOneTrampoline(IRFunction& function) {
    for (BlockId id = 0; id < function.blocks.size(); ++id) {
        if (id == function.entry) continue;
        const auto& candidate = function.blocks[id];
        if (!candidate.instructions.empty() || candidate.predecessors.empty()) continue;
        const auto* jump = std::get_if<IRJump>(&*candidate.terminator);
        if (!jump || jump->target == id) continue;
        const BlockId target = jump->target;
        bool ambiguous = false;
        for (const BlockId predecessor : candidate.predecessors)
            if (std::find(function.blocks[predecessor].successors.begin(),
                         function.blocks[predecessor].successors.end(), target) !=
                function.blocks[predecessor].successors.end())
                ambiguous = true;
        if (ambiguous) continue;

        const auto predecessors = candidate.predecessors;
        for (auto& instruction : function.blocks[target].instructions) {
            if (instruction.op != IROp::Phi) break;
            const auto input = std::find_if(instruction.phiInputs.begin(),
                                            instruction.phiInputs.end(),
                                            [&](const PhiInput& value) {
                                                return value.predecessor == id;
                                            });
            if (input == instruction.phiInputs.end()) continue;
            const ValueId value = input->value;
            instruction.phiInputs.erase(input);
            for (const BlockId predecessor : predecessors)
                instruction.phiInputs.push_back({predecessor, value});
        }
        for (const BlockId predecessor : predecessors)
            redirect(*function.blocks[predecessor].terminator, id, target);
        function.blocks[id].terminator = IRUnreachable{};
        canonicalizeIR(function);
        return true;
    }
    return false;
}

bool mergeOneLinearBlock(IRFunction& function) {
    for (BlockId from = 0; from < function.blocks.size(); ++from) {
        auto* jump = std::get_if<IRJump>(&*function.blocks[from].terminator);
        if (!jump || jump->target == from) continue;
        const BlockId to = jump->target;
        if (to == function.entry || function.blocks[from].successors.size() != 1 ||
            function.blocks[to].predecessors.size() != 1 ||
            function.blocks[to].predecessors[0] != from)
            continue;

        auto& destination = function.blocks[to];
        while (!destination.instructions.empty() &&
               destination.instructions.front().op == IROp::Phi) {
            const auto phi = destination.instructions.front();
            if (phi.phiInputs.size() != 1) break;
            replaceAllUses(function, *phi.result, phi.phiInputs[0].value);
            destination.instructions.erase(destination.instructions.begin());
        }
        auto moved = std::move(destination.instructions);
        auto terminator = std::move(destination.terminator);
        const auto successors = destination.successors;
        auto& source = function.blocks[from];
        source.instructions.insert(source.instructions.end(),
                                   std::make_move_iterator(moved.begin()),
                                   std::make_move_iterator(moved.end()));
        source.terminator = std::move(terminator);
        for (const BlockId successor : successors)
            for (auto& instruction : function.blocks[successor].instructions) {
                if (instruction.op != IROp::Phi) break;
                for (auto& input : instruction.phiInputs)
                    if (input.predecessor == to) input.predecessor = from;
            }
        destination.instructions.clear();
        destination.terminator = IRUnreachable{};
        canonicalizeIR(function);
        return true;
    }
    return false;
}

} // namespace

PassResult runSimplifyCFG(IRFunction& function) {
    PassResult result;
    const auto initialBlocks = function.blocks.size();
    while (true) {
        bool changed = false;
        changed = foldOneBranch(function) || changed;
        const auto beforeUnreachable = function.blocks.size();
        changed = removeUnreachableIR(function) || changed;
        result.blocksRemoved += beforeUnreachable - function.blocks.size();
        changed = eliminateTrivialPhis(function) || changed;
        changed = bypassOneTrampoline(function) || changed;
        changed = mergeOneLinearBlock(function) || changed;
        result.changed = result.changed || changed;
        if (!changed) break;
    }
    canonicalizeIR(function);
    result.blocksRemoved = std::max(result.blocksRemoved,
                                    initialBlocks - function.blocks.size());
    return result;
}

} // namespace toyc
