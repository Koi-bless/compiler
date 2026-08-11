#include <algorithm>

#include "test_support.hpp"
#include "toyc/ir/dominator.hpp"

int main() {
    TestPipeline pipeline("int main(){int x=0;if(x)x=1;else x=2;return x;}");
    const auto& function = pipeline.cfg.functions[0];
    toyc::DominatorInfo dominators(function);
    check(dominators.immediateDominator(function.entry) == function.entry, "entry does not dominate itself");
    check(function.blocks[function.entry].successors.size() == 2, "diamond was not formed");
    const auto left = function.blocks[function.entry].successors[0];
    const auto right = function.blocks[function.entry].successors[1];
    check(dominators.dominates(function.entry, left) && dominators.dominates(function.entry, right),
          "entry does not dominate both diamond arms");
    const auto merge = function.blocks[left].successors[0];
    check(std::find(dominators.frontier(left).begin(), dominators.frontier(left).end(), merge) != dominators.frontier(left).end(),
          "left dominance frontier lacks merge");
    check(std::find(dominators.frontier(right).begin(), dominators.frontier(right).end(), merge) != dominators.frontier(right).end(),
          "right dominance frontier lacks merge");
}
