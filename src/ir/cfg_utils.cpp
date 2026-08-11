#include "toyc/ir/cfg_utils.hpp"

#include <algorithm>
#include <functional>
#include <queue>

#include "toyc/support/diagnostic.hpp"

namespace toyc {

std::vector<bool> computeReachable(const CFGFunction& function) {
    std::vector<bool> result(function.blocks.size(), false);
    if (function.entry >= function.blocks.size()) return result;
    std::queue<BlockId> work;
    result[function.entry] = true;
    work.push(function.entry);
    while (!work.empty()) {
        const BlockId block = work.front(); work.pop();
        for (const BlockId successor : function.blocks[block].successors)
            if (successor < result.size() && !result[successor]) {
                result[successor] = true; work.push(successor);
            }
    }
    return result;
}

std::vector<BlockId> computeReversePostOrder(const CFGFunction& function) {
    std::vector<BlockId> postorder;
    std::vector<bool> visited(function.blocks.size(), false);
    std::function<void(BlockId)> visit = [&](BlockId block) {
        visited[block] = true;
        for (const BlockId successor : function.blocks[block].successors)
            if (successor < visited.size() && !visited[successor]) visit(successor);
        postorder.push_back(block);
    };
    if (function.entry < function.blocks.size()) visit(function.entry);
    std::reverse(postorder.begin(), postorder.end());
    return postorder;
}

void rebuildPredecessors(CFGFunction& function) {
    for (auto& block : function.blocks) block.predecessors.clear();
    for (const auto& block : function.blocks)
        for (const BlockId successor : block.successors)
            function.blocks[successor].predecessors.push_back(block.id);
    for (auto& block : function.blocks) {
        std::sort(block.predecessors.begin(), block.predecessors.end());
        block.predecessors.erase(std::unique(block.predecessors.begin(), block.predecessors.end()),
                                 block.predecessors.end());
    }
}

bool removeUnreachable(CFGFunction& function) {
    const auto reachable = computeReachable(function);
    if (std::all_of(reachable.begin(), reachable.end(), [](bool value) { return value; }))
        return false;
    const BlockId invalidId = static_cast<BlockId>(function.blocks.size());
    std::vector<BlockId> mapping(function.blocks.size(), invalidId);
    std::vector<CFGBlock> blocks;
    for (const auto& block : function.blocks) if (reachable[block.id]) {
        mapping[block.id] = static_cast<BlockId>(blocks.size());
        blocks.push_back(block);
        blocks.back().id = static_cast<BlockId>(blocks.size() - 1);
    }
    for (auto& block : blocks) {
        if (auto* jump = std::get_if<Jump>(&*block.terminator)) jump->target = mapping[jump->target];
        else if (auto* branch = std::get_if<Branch>(&*block.terminator)) {
            branch->trueTarget = mapping[branch->trueTarget];
            branch->falseTarget = mapping[branch->falseTarget];
        }
        for (auto& successor : block.successors) successor = mapping[successor];
        std::sort(block.successors.begin(), block.successors.end());
        block.successors.erase(std::unique(block.successors.begin(), block.successors.end()), block.successors.end());
    }
    function.entry = mapping[function.entry];
    function.blocks = std::move(blocks);
    rebuildPredecessors(function);
    return true;
}

bool removeUnreachable(CFGModule& module) {
    bool changed = false;
    for (auto& function : module.functions) changed = removeUnreachable(function) || changed;
    return changed;
}

void replaceSuccessor(CFGFunction& function, BlockId from,
                      BlockId oldTarget, BlockId newTarget) {
    auto& block = function.blocks.at(from);
    if (auto* jump = std::get_if<Jump>(&*block.terminator)) {
        if (jump->target == oldTarget) jump->target = newTarget;
    } else if (auto* branch = std::get_if<Branch>(&*block.terminator)) {
        if (branch->trueTarget == oldTarget) branch->trueTarget = newTarget;
        if (branch->falseTarget == oldTarget) branch->falseTarget = newTarget;
    }
    for (auto& successor : block.successors)
        if (successor == oldTarget) successor = newTarget;
    std::sort(block.successors.begin(), block.successors.end());
    block.successors.erase(std::unique(block.successors.begin(), block.successors.end()), block.successors.end());
    rebuildPredecessors(function);
}

BlockId splitEdge(IRFunction& function, BlockId from, BlockId to) {
    if (from >= function.blocks.size() || to >= function.blocks.size())
        throw CompileError(function.location, "SSA transformation", "cannot split an invalid edge");
    auto& source = function.blocks[from];
    bool replaced = false;
    const BlockId split = static_cast<BlockId>(function.blocks.size());
    if (auto* jump = std::get_if<IRJump>(&*source.terminator); jump && jump->target == to) {
        jump->target = split; replaced = true;
    } else if (auto* branch = std::get_if<BranchValue>(&*source.terminator)) {
        if (branch->trueTarget == to) { branch->trueTarget = split; replaced = true; }
        if (branch->falseTarget == to) { branch->falseTarget = split; replaced = true; }
    }
    if (!replaced) throw CompileError(function.location, "SSA transformation", "edge is absent from terminator");
    for (auto& successor : source.successors) if (successor == to) successor = split;
    auto& target = function.blocks[to];
    for (auto& predecessor : target.predecessors) if (predecessor == from) predecessor = split;
    for (auto& instruction : target.instructions) {
        if (instruction.op != IROp::Phi) break;
        for (auto& input : instruction.phiInputs)
            if (input.predecessor == from) input.predecessor = split;
        std::sort(instruction.phiInputs.begin(), instruction.phiInputs.end(),
                  [](const PhiInput& a, const PhiInput& b) { return a.predecessor < b.predecessor; });
    }
    IRBlock block;
    block.id = split;
    block.predecessors = {from};
    block.successors = {to};
    block.terminator = IRJump{to};
    function.blocks.push_back(std::move(block));
    return split;
}

} // namespace toyc
