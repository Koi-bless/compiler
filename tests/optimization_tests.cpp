#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>

#include "test_support.hpp"
#include "toyc/ir/ir_printer.hpp"
#include "toyc/opt/dce.hpp"
#include "toyc/opt/function_effects.hpp"
#include "toyc/opt/ir_utils.hpp"
#include "toyc/opt/loop_analysis.hpp"
#include "toyc/opt/loop_summary.hpp"

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

std::optional<std::int32_t> returnedConstant(const toyc::IRFunction& function) {
    for (const auto& block : function.blocks) {
        const auto* returned = std::get_if<toyc::ReturnValue>(&*block.terminator);
        if (!returned || !returned->value) continue;
        const auto* definition = toyc::findDefinition(function, *returned->value);
        if (definition && definition->op == toyc::IROp::Constant)
            return definition->immediate;
    }
    return std::nullopt;
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
    check(countOp(optimized.ir, toyc::IROp::LoadGlobal) >= 2,
          "global loads were incorrectly combined across a call");
    check(countOp(optimized.ir, toyc::IROp::StoreGlobal) >= 1,
          "global store was deleted");
}

void testImmutableGlobalPropagation() {
    TestPipeline immutable(R"(
        int scale = 17;
        int bias = 29;
        int main() { return scale * 2 + bias; }
    )", true);
    check(countOp(immutable.ir, toyc::IROp::LoadGlobal) == 0,
          "never-written globals were not propagated");
    check(returnedConstant(immutable.ir.functions[0]) == 63,
          "propagated immutable globals were not folded");

    TestPipeline mutableGlobal(R"(
        int state = 3;
        int main() { state = state + 1; return state; }
    )", true);
    check(countOp(mutableGlobal.ir, toyc::IROp::LoadGlobal) != 0 &&
          countOp(mutableGlobal.ir, toyc::IROp::StoreGlobal) != 0,
          "written global was incorrectly treated as immutable");
}

void testFunctionEffectsAndCallDCE() {
    TestPipeline pure(R"(
        int dead(int x) {
            if (x < 0) return 0 - x;
            return x * 3 + 1;
        }
        int main() { dead(7); return 4; }
    )");
    const auto pureEffects = toyc::analyzeFunctionEffects(pure.ir);
    check(pureEffects.functions[0].removableCall(),
          "acyclic pure helper was not proven removable");
    toyc::runDCE(pure.ir.functions[1], true, &pureEffects);
    check(countOp(pure.ir, toyc::IROp::Call) == 0,
          "unused pure call was not removed");

    TestPipeline trapping(R"(
        int divide(int x, int y) { return x / y; }
        int main() { divide(7, 0); return 4; }
    )");
    const auto trapEffects = toyc::analyzeFunctionEffects(trapping.ir);
    toyc::runDCE(trapping.ir.functions[1], true, &trapEffects);
    check(countOp(trapping.ir, toyc::IROp::Call) == 1,
          "potentially trapping call was incorrectly removed");

    TestPipeline recursive(R"(
        int recurse(int x) {
            if (x == 0) return 0;
            return recurse(x - 1);
        }
        int main() { recurse(3); return 4; }
    )");
    const auto recursiveEffects = toyc::analyzeFunctionEffects(recursive.ir);
    toyc::runDCE(recursive.ir.functions[1], true, &recursiveEffects);
    check(countOp(recursive.ir, toyc::IROp::Call) == 2,
          "recursive call was incorrectly treated as proven terminating");
}

void testMultiBlockInlining() {
    TestPipeline optimized(R"(
        int absolute(int x) {
            if (x < 0) return 0 - x;
            return x;
        }
        int main() { return absolute(-9); }
    )", true);
    check(countOp(optimized.ir, toyc::IROp::Call) == 0,
          "branching helper was not inlined");
    check(returnedConstant(optimized.ir.functions[1]) == 9,
          "multiple-return CFG inlining produced the wrong value");
    toyc::verifyIR(optimized.ir, optimized.semantic);
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

void testExactLoopFinalValues() {
    TestPipeline zero(R"(
        int main() {
            int i = 5;
            int result = 42;
            while (i < 5) { result = 99; i = i + 1; }
            return result;
        }
    )", true);
    check(toyc::analyzeLoops(zero.ir.functions[0]).empty(),
          "zero-trip exact loop was not deleted");
    check(returnedConstant(zero.ir.functions[0]) == 42,
          "zero-trip loop did not preserve its initial live-out value");

    TestPipeline positive(R"(
        int main() {
            int i = 0;
            int result = 9;
            while (i < 1000000000) { result = i * 3 + 1; i = i + 1; }
            return result;
        }
    )", true);
    check(toyc::analyzeLoops(positive.ir.functions[0]).empty(),
          "finite overwrite loop was not deleted");
    check(returnedConstant(positive.ir.functions[0]) == -1294967298,
          "finite overwrite loop has the wrong wrapped final value");

    TestPipeline descending(R"(
        int main() {
            int i = 10;
            int result = 0;
            while (i > 0) { result = i; i = i - 2; }
            return result;
        }
    )", true);
    check(toyc::analyzeLoops(descending.ir.functions[0]).empty(),
          "negative-step loop was not deleted");
    check(returnedConstant(descending.ir.functions[0]) == 2,
          "negative-step final value is incorrect");
}

void testAffineLoopRecurrences() {
    TestPipeline additive(R"(
        int main() {
            int i = 0;
            int sum = 0;
            while (i < 1000000) { sum = sum + i; i = i + 1; }
            return sum;
        }
    )", true);
    check(toyc::analyzeLoops(additive.ir.functions[0]).empty(),
          "additive reduction loop was not deleted");
    check(returnedConstant(additive.ir.functions[0]) == 1783293664,
          "additive reduction has the wrong wrapped final value");

    std::int32_t expectedAffine = 7;
    for (std::int32_t i = 0; i < 64; ++i)
        expectedAffine = toyc::wrapAdd(toyc::wrapMul(expectedAffine, 17), i);
    TestPipeline affine(R"(
        int main() {
            int i = 0;
            int state = 7;
            while (i < 64) { state = state * 17 + i; i = i + 1; }
            return state;
        }
    )", true);
    check(toyc::analyzeLoops(affine.ir.functions[0]).empty(),
          "affine recurrence loop was not deleted");
    check(returnedConstant(affine.ir.functions[0]) == expectedAffine,
          "affine recurrence has the wrong wrapped final value");

    std::int32_t a = 1;
    std::int32_t b = 1;
    for (int i = 0; i < 1000000; ++i) {
        const std::int32_t next = toyc::wrapAdd(a, b);
        a = b;
        b = next;
    }
    TestPipeline coupled(R"(
        int main() {
            int i = 0;
            int a = 1;
            int b = 1;
            while (i < 1000000) {
                int next = a + b;
                a = b;
                b = next;
                i = i + 1;
            }
            return b;
        }
    )", true);
    check(toyc::analyzeLoops(coupled.ir.functions[0]).empty(),
          "coupled linear recurrence loop was not deleted");
    check(returnedConstant(coupled.ir.functions[0]) == b,
          "coupled linear recurrence has the wrong wrapped final value");
}

void testFiniteLoopFunctionEffects() {
    const std::string finiteSource = R"(
        int unused() {
            int i = 0;
            int sum = 0;
            while (i < 1000000) { sum = sum + i; i = i + 1; }
            return sum;
        }
        int main() { unused(); return 4; }
    )";
    TestPipeline finite(finiteSource);
    const auto effects = toyc::analyzeFunctionEffects(finite.ir);
    check(effects.functions[0].removableCall(),
          "pure exact-count loop was not proven terminating");
    toyc::runDCE(finite.ir.functions[1], true, &effects);
    check(countOp(finite.ir, toyc::IROp::Call) == 0,
          "unused call to a finite pure loop was not deleted");
    TestPipeline optimizedFinite(finiteSource, true);
    check(countOp(optimizedFinite.ir, toyc::IROp::Call) == 0,
          "optimization pipeline retained an unused finite pure loop call");

    TestPipeline unknown(R"(
        int maybeForever(int n) {
            while (n != 0) n = n + 1;
            return n;
        }
        int main() { maybeForever(1); return 4; }
    )");
    const auto unknownEffects = toyc::analyzeFunctionEffects(unknown.ir);
    check(!unknownEffects.functions[0].removableCall(),
          "unproven loop was incorrectly treated as terminating");
    toyc::runDCE(unknown.ir.functions[1], true, &unknownEffects);
    check(countOp(unknown.ir, toyc::IROp::Call) == 1,
          "call to a potentially nonterminating loop was deleted");
}

void testLoopDeletionSafety() {
    TestPipeline overflow(R"(
        int main() {
            int i = 2147483646;
            while (i <= 2147483647) i = i + 1;
            return i;
        }
    )", true);
    check(!toyc::analyzeLoops(overflow.ir.functions[0]).empty(),
          "overflowing induction loop was incorrectly deleted");

    TestPipeline sideEffect(R"(
        int g = 0;
        int main() {
            int i = 0;
            while (i < 4) { g = i; i = i + 1; }
            return g;
        }
    )", true);
    check(!toyc::analyzeLoops(sideEffect.ir.functions[0]).empty(),
          "loop containing a global store was incorrectly deleted");
}

void testPreciseNonTrappingDCE() {
    TestPipeline safe("int f(int x) { int dead = x % 7; return x; } int main() { return f(3); }", true);
    check(countOp(safe.ir, toyc::IROp::SRem) == 0,
          "dead remainder by a known nonzero divisor remains");

    TestPipeline unsafe("int f(int x, int y) { int dead = x % y; return x; } int main() { return f(3, 1); }", true);
    check(countOp(unsafe.ir, toyc::IROp::SRem) == 1,
          "potentially trapping remainder was incorrectly deleted");
}

void testInliningAndReadOnlyLoopCollapse() {
    TestPipeline pure(R"(
        int step(int x) { return (x % 31) * 3 + 5; }
        int main() {
            int i = 0;
            int result = 0;
            while (i < 1000000) { result = step(i); i = i + 1; }
            return result;
        }
    )", true);
    check(toyc::analyzeLoops(pure.ir.functions[1]).empty(),
          "pure helper loop was not collapsed after inlining");
    check(countOp(pure.ir, toyc::IROp::Call) == 0,
          "small leaf helper was not inlined");

    TestPipeline readOnly(R"(
        int scale = 3;
        int modulus = 97;
        int apply(int x) { return (x * scale) % modulus; }
        int main() {
            int i = 0;
            int result = 0;
            while (i < 1000) { result = apply(i); i = i + 1; }
            return result;
        }
    )", true);
    check(toyc::analyzeLoops(readOnly.ir.functions[1]).empty(),
          "read-only helper loop was not collapsed");
    check(countOp(readOnly.ir, toyc::IROp::LoadGlobal) == 0,
          "never-written globals were not propagated through the collapsed loop");
    check(returnedConstant(readOnly.ir.functions[1]) == 87,
          "immutable-global loop collapse produced the wrong result");
}

void testNestedConstantControlLoopCollapse() {
    TestPipeline nested(R"(
        int main() {
            int round = 0;
            int result = 0;
            while (round < 1000000) {
                int a = round % 17;
                int b = 1000;
                int pass = 0;
                while (pass < 3) {
                    int candidate = a + 7;
                    if (candidate < b) b = candidate;
                    candidate = b + 2;
                    if (candidate < a) a = candidate;
                    pass = pass + 1;
                }
                result = a + b;
                round = round + 1;
            }
            return result;
        }
    )", true);
    check(toyc::analyzeLoops(nested.ir.functions[0]).empty(),
          "bounded nested constant-control loop was not collapsed");
    check(returnedConstant(nested.ir.functions[0]).has_value(),
          "nested loop final value was not folded to a constant");
}

void testTailRecursionElimination() {
    TestPipeline tail(R"(
        int sum(int n, int acc) {
            if (n == 0) return acc;
            return sum(n - 1, acc + n);
        }
        int main() { return sum(20, 0); }
    )", true);
    check(countOp(tail.ir, toyc::IROp::Call) == 1,
          "direct tail recursion was not eliminated");
    check(!toyc::analyzeLoops(tail.ir.functions[0]).empty(),
          "tail recursion was not rewritten as a loop");
    check(toyc::analyzeLoops(tail.ir.functions[1]).empty(),
          "caller loop around a proven terminating pure tail loop remains");

    TestPipeline nonTail(R"(
        int factorial(int n) {
            if (n == 0) return 1;
            return n * factorial(n - 1);
        }
        int main() { return factorial(5); }
    )", true);
    check(countOp(nonTail.ir, toyc::IROp::Call) == 2,
          "non-tail recursion was incorrectly transformed");

    TestPipeline nonTerminating(R"(
        int spin(int n) {
            while (n != 0) n = n + 1;
            return n;
        }
        int main() {
            int i = 0;
            int result = 0;
            while (i < 4) { result = spin(1); i = i + 1; }
            return result;
        }
    )", true);
    check(!toyc::analyzeLoops(nonTerminating.ir.functions[1]).empty(),
          "loop containing a potentially nonterminating pure call was deleted");
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
        testImmutableGlobalPropagation();
        testFunctionEffectsAndCallDCE();
        testMultiBlockInlining();
        testCompareBranchFusion();
        testLoopInvariantHoisting();
        testExactLoopFinalValues();
        testAffineLoopRecurrences();
        testFiniteLoopFunctionEffects();
        testLoopDeletionSafety();
        testPreciseNonTrappingDCE();
        testInliningAndReadOnlyLoopCollapse();
        testNestedConstantControlLoopCollapse();
        testTailRecursionElimination();
        testPipelineIdempotence();
        std::cout << "optimization tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
