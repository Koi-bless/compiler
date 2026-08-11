#pragma once

#include <vector>

#include "toyc/ir/cfg.hpp"
#include "toyc/ir/ir.hpp"

namespace toyc {

std::vector<bool> computeReachable(const CFGFunction& function);
std::vector<BlockId> computeReversePostOrder(const CFGFunction& function);
bool removeUnreachable(CFGFunction& function);
bool removeUnreachable(CFGModule& module);
void rebuildPredecessors(CFGFunction& function);
void replaceSuccessor(CFGFunction& function, BlockId from,
                      BlockId oldTarget, BlockId newTarget);
BlockId splitEdge(IRFunction& function, BlockId from, BlockId to);

} // namespace toyc
