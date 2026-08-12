#include "toyc/opt/loop_analysis.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <set>

#include "toyc/ir/dominator.hpp"

namespace toyc {

std::vector<Loop> analyzeLoops(const IRFunction& function) {
    DominatorInfo dominators(function);
    std::map<BlockId, std::set<BlockId>> blocksByHeader;
    std::map<BlockId, std::set<BlockId>> latchesByHeader;
    for (const auto& tail : function.blocks) {
        for (const BlockId header : tail.successors) {
            if (!dominators.dominates(header, tail.id)) continue;
            auto& loopBlocks = blocksByHeader[header];
            loopBlocks.insert(header);
            loopBlocks.insert(tail.id);
            latchesByHeader[header].insert(tail.id);
            std::vector<BlockId> work{tail.id};
            while (!work.empty()) {
                const BlockId block = work.back();
                work.pop_back();
                if (block == header) continue;
                for (const BlockId predecessor : function.blocks[block].predecessors)
                    if (loopBlocks.insert(predecessor).second) work.push_back(predecessor);
            }
        }
    }
    std::vector<Loop> loops;
    for (const auto& [header, blockSet] : blocksByHeader) {
        Loop loop;
        loop.header = header;
        loop.blocks.assign(blockSet.begin(), blockSet.end());
        loop.latches.assign(latchesByHeader[header].begin(), latchesByHeader[header].end());
        std::vector<BlockId> outside;
        for (const BlockId predecessor : function.blocks[header].predecessors)
            if (!blockSet.contains(predecessor)) outside.push_back(predecessor);
        if (outside.size() == 1 && function.blocks[outside[0]].successors.size() == 1)
            loop.preheader = outside[0];
        loops.push_back(std::move(loop));
    }
    for (std::size_t child = 0; child < loops.size(); ++child) {
        std::optional<std::size_t> parent;
        for (std::size_t candidate = 0; candidate < loops.size(); ++candidate) {
            if (child == candidate || loops[candidate].blocks.size() <= loops[child].blocks.size())
                continue;
            if (!std::includes(loops[candidate].blocks.begin(), loops[candidate].blocks.end(),
                               loops[child].blocks.begin(), loops[child].blocks.end()))
                continue;
            if (!parent || loops[candidate].blocks.size() < loops[*parent].blocks.size())
                parent = candidate;
        }
        loops[child].parent = parent;
    }
    std::function<unsigned(std::size_t)> depth = [&](std::size_t index) {
        if (!loops[index].parent) return 1U;
        return depth(*loops[index].parent) + 1U;
    };
    for (std::size_t index = 0; index < loops.size(); ++index)
        loops[index].depth = depth(index);
    return loops;
}

} // namespace toyc
