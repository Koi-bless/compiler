#pragma once

#include <vector>

#include "toyc/ir/cfg.hpp"

namespace toyc {

class DominatorInfo {
public:
    explicit DominatorInfo(const CFGFunction& function);
    BlockId immediateDominator(BlockId block) const;
    bool dominates(BlockId a, BlockId b) const;
    const std::vector<BlockId>& children(BlockId block) const;
    const std::vector<BlockId>& frontier(BlockId block) const;
    const std::vector<BlockId>& reversePostOrder() const { return rpo_; }

private:
    BlockId entry_{};
    std::vector<BlockId> idom_;
    std::vector<std::vector<BlockId>> children_;
    std::vector<std::vector<BlockId>> frontier_;
    std::vector<BlockId> rpo_;
};

} // namespace toyc
