#include "toyc/ir/dominator.hpp"

#include <algorithm>
#include <limits>

#include "toyc/ir/cfg_utils.hpp"
#include "toyc/opt/ir_utils.hpp"
#include "toyc/support/diagnostic.hpp"

namespace toyc {

DominatorInfo::DominatorInfo(const CFGFunction& function)
    : rpo_(computeReversePostOrder(function)) {
    if (rpo_.size() != function.blocks.size())
        throw CompileError(function.location, "dominator", "unreachable blocks must be removed before analysis");
    std::vector<std::vector<BlockId>> predecessors;
    predecessors.reserve(function.blocks.size());
    for (const auto& block : function.blocks) predecessors.push_back(block.predecessors);
    initialize(function.entry, predecessors);
}

DominatorInfo::DominatorInfo(const IRFunction& function)
    : rpo_(computeReversePostOrder(function)) {
    if (rpo_.size() != function.blocks.size())
        throw CompileError(function.location, "dominator", "unreachable blocks must be removed before analysis");
    std::vector<std::vector<BlockId>> predecessors;
    predecessors.reserve(function.blocks.size());
    for (const auto& block : function.blocks) predecessors.push_back(block.predecessors);
    initialize(function.entry, predecessors);
}

void DominatorInfo::initialize(
    BlockId entry, const std::vector<std::vector<BlockId>>& predecessors) {
    entry_ = entry;
    idom_.assign(predecessors.size(), std::numeric_limits<BlockId>::max());
    children_.assign(predecessors.size(), {});
    frontier_.assign(predecessors.size(), {});
    std::vector<std::size_t> order(predecessors.size());
    for (std::size_t index = 0; index < rpo_.size(); ++index) order[rpo_[index]] = index;
    idom_[entry_] = entry_;
    const auto intersect = [&](BlockId left, BlockId right) {
        while (left != right) {
            while (order[left] > order[right]) left = idom_[left];
            while (order[right] > order[left]) right = idom_[right];
        }
        return left;
    };
    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t index = 1; index < rpo_.size(); ++index) {
            const BlockId block = rpo_[index];
            BlockId next = std::numeric_limits<BlockId>::max();
            for (const BlockId predecessor : predecessors[block]) {
                if (idom_[predecessor] == std::numeric_limits<BlockId>::max()) continue;
                next = next == std::numeric_limits<BlockId>::max() ? predecessor : intersect(predecessor, next);
            }
            if (next != idom_[block]) { idom_[block] = next; changed = true; }
        }
    }
    for (BlockId block = 0; block < predecessors.size(); ++block)
        if (block != entry_) children_[idom_[block]].push_back(block);
    for (auto& values : children_) std::sort(values.begin(), values.end());
    for (BlockId block = 0; block < predecessors.size(); ++block)
        if (predecessors[block].size() >= 2) {
        for (const BlockId predecessor : predecessors[block]) {
            BlockId runner = predecessor;
            while (runner != idom_[block]) {
                frontier_[runner].push_back(block);
                runner = idom_[runner];
            }
        }
    }
    for (auto& values : frontier_) {
        std::sort(values.begin(), values.end());
        values.erase(std::unique(values.begin(), values.end()), values.end());
    }
}

BlockId DominatorInfo::immediateDominator(BlockId block) const { return idom_.at(block); }

bool DominatorInfo::dominates(BlockId a, BlockId b) const {
    if (a >= idom_.size() || b >= idom_.size()) return false;
    while (b != entry_ && b != a) b = idom_[b];
    return b == a;
}

const std::vector<BlockId>& DominatorInfo::children(BlockId block) const { return children_.at(block); }
const std::vector<BlockId>& DominatorInfo::frontier(BlockId block) const { return frontier_.at(block); }

} // namespace toyc
