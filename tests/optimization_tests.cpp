#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>

#include "test_support.hpp"
#include "toyc/ir/ir_printer.hpp"

namespace {

std::size_t countOp(const toyc::IRModule& module, toyc::IROp op) {
    std::size_t count = 0;
    for (const auto& function : module.functions)
        for (const auto& block : function.blocks)
            count += static_cast<std::size_t>(std::count_if(
                block.instructions.begin(), block.instructions.end(),
                [&](const toyc::IRInstruction& instruction) {
                    return instruction.op == op;
                }));
    return count;
}

void testConstantBranchAndDeadCode() {
    const std::string source = R"(
        int main() {
            int x = 2 * 3;
            int y = x + 0;
            if (1) y = y + 4;
            else y = y + 100;
            return y;
        }
    )";
    TestPipeline normal(source);
    TestPipeline optimized(source, true);
    check(optimized.ir.functions[0].blocks.size() < normal.ir.functions[0].blocks.size(),
          "constant branch was not removed");
    check(countOp(optimized.ir, toyc::IROp::Mul) == 0,
          "constant multiplication remains");
    check(countOp(optimized.ir, toyc::IROp::Add) == 0,
          "foldable/dead additions remain");
    toyc::verifyIR(optimized.ir, optimized.semantic);
}

void testRedundantExpression() {
    const std::string source = R"(
        int sum(int a, int b) {
            int x = a + b;
            int y = b + a;
            return x + y;
        }
        int main() { return sum(3, 4); }
    )";
    TestPipeline normal(source);
    TestPipeline optimized(source, true);
    check(countOp(optimized.ir, toyc::IROp::Add) < countOp(normal.ir, toyc::IROp::Add),
          "redundant commutative expression was not eliminated");
}

void testGlobalAndCallBarrier() {
    const std::string source = R"(
        int g = 1;
        int update() { g = g + 1; return g; }
        int main() {
            int before = g;
            update();
            int after = g;
            return before + after;
        }
    )";
    TestPipeline optimized(source, true);
    check(countOp(optimized.ir, toyc::IROp::Call) == 1,
          "unused result call was deleted");
    check(countOp(optimized.ir, toyc::IROp::LoadGlobal) >= 2,
          "global loads were incorrectly combined across a call");
    check(countOp(optimized.ir, toyc::IROp::StoreGlobal) == 1,
          "global store was deleted");
}

void testCompareBranchFusion() {
    const std::string source = R"(
        int less(int a, int b) {
            if (a < b) return 1;
            return 0;
        }
        int main() { return less(2, 3); }
    )";
    TestPipeline optimized(source, true);
    bool fused = false;
    for (const auto& function : optimized.machine.functions)
        for (const auto& block : function.blocks)
            for (const auto& instruction : block.instructions)
                fused = fused || instruction.opcode == toyc::MOpcode::BLT;
    check(fused, "single-use compare was not fused with its branch");
}

void testLoopInvariantHoisting() {
    const std::string source = R"(
        int loop(int a, int b, int n) {
            int i = 0;
            int total = 0;
            while (i < n) {
                int invariant = a + b;
                total = total + invariant;
                i = i + 1;
            }
            return total;
        }
        int main() { return loop(2, 3, 4); }
    )";
    TestPipeline optimized(source, true);
    const auto& function = optimized.ir.functions[0];
    bool entryContainsInvariantAdd = false;
    for (const auto& instruction : function.blocks[function.entry].instructions)
        entryContainsInvariantAdd = entryContainsInvariantAdd ||
            (instruction.op == toyc::IROp::Add && instruction.operands.size() == 2);
    check(entryContainsInvariantAdd, "safe loop-invariant add was not hoisted");
}

void testPipelineIdempotence() {
    TestPipeline pipeline("int main() { int x = 4 + 5; return x * 1; }", true);
    std::ostringstream before;
    toyc::printIR(before, pipeline.ir, pipeline.semantic);
    toyc::OptimizationOptions options;
    options.enabled = true;
    options.verifyEach = true;
    std::ostringstream diagnostics;
    toyc::runOptimizationPipeline(pipeline.ir, pipeline.semantic, options, diagnostics);
    std::ostringstream after;
    toyc::printIR(after, pipeline.ir, pipeline.semantic);
    check(before.str() == after.str(), "optimization pipeline is not idempotent");
}

} // namespace

int main() {
    try {
        testConstantBranchAndDeadCode();
        testRedundantExpression();
        testGlobalAndCallBarrier();
        testCompareBranchFusion();
        testLoopInvariantHoisting();
        testPipelineIdempotence();
        std::cout << "optimization tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
