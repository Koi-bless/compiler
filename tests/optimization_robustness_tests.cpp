#include <algorithm>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>

#include "test_support.hpp"
#include "toyc/opt/function_effects.hpp"
#include "toyc/opt/ir_utils.hpp"
#include "toyc/opt/loop_analysis.hpp"

namespace {

std::size_t countOp(const toyc::IRFunction& function, toyc::IROp op) {
    std::size_t count = 0;
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

void expectConstantReturn(const toyc::IRFunction& function,
                          std::int32_t expected,
                          const std::string& context) {
    const auto actual = returnedConstant(function);
    check(actual.has_value(), context + ": return value was not folded");
    check(*actual == expected, context + ": wrong folded return value");
}

void testTransitivePureEffectsRemoveDeadHotCalls() {
    const std::string source = R"(
        int leaf(int x) {
            if (x < 0) return 0 - x;
            return x * 3 + 1;
        }
        int middle(int x) { return leaf(x) + 5; }
        int top(int x) { return middle(x) * 7; }
        int main() {
            int i = 0;
            int result = -1;
            while (i < 1000000) {
                int dead0 = top(i);
                int dead1 = top(i + 1);
                result = i;
                i = i + 1;
            }
            return result;
        }
    )";

    TestPipeline baseline(source);
    const auto effects = toyc::analyzeFunctionEffects(baseline.ir);
    check(effects.functions.size() >= 4,
          "transitive effects: incomplete function summary table");
    check(effects.functions[0].removableCall() &&
              effects.functions[1].removableCall() &&
              effects.functions[2].removableCall(),
          "transitive effects: pure wrapper chain was not removable");

    TestPipeline optimized(source, true);
    const auto& mainFunction = optimized.ir.functions[3];
    check(countOp(mainFunction, toyc::IROp::Call) == 0,
          "transitive effects: dead calls remain in main");
    check(toyc::analyzeLoops(mainFunction).empty(),
          "transitive effects: dead-call loop was not deleted");
    expectConstantReturn(mainFunction, 999999, "transitive effects");
}

void testNestedCfgInliningAndCleanup() {
    TestPipeline optimized(R"(
        int classify(int x) {
            int adjusted = x + 3;
            if (adjusted < 0) return 0 - adjusted;
            if (adjusted == 0) return 11;
            return adjusted * 2;
        }
        int forward(int x) {
            int value = classify(x);
            if (value < 10) return value + 1;
            return value - 1;
        }
        int main() {
            int left = forward(-8);
            int right = forward(2);
            return left + right;
        }
    )", true);

    const auto& mainFunction = optimized.ir.functions[2];
    check(countOp(mainFunction, toyc::IROp::Call) == 0,
          "nested CFG inlining: calls remain in main");
    expectConstantReturn(mainFunction, 15, "nested CFG inlining");
    toyc::verifyIR(optimized.ir, optimized.semantic);
}

void testRepeatedPureCallsThroughWrapper() {
    TestPipeline optimized(R"(
        int normalize(int x) {
            if (x < 0) x = 0 - x;
            return x % 31;
        }
        int wrapped(int x, int bias) {
            int value = normalize(x);
            if (bias < 0) return value - bias;
            return value + bias;
        }
        int main() {
            int i = 0;
            int result = 0;
            while (i < 2000000) {
                int a = wrapped(i, i % 7);
                int b = wrapped(i, i % 7);
                int c = wrapped(i, i % 7);
                result = a + b - c;
                i = i + 1;
            }
            return result;
        }
    )", true);

    const auto& mainFunction = optimized.ir.functions[2];
    check(countOp(mainFunction, toyc::IROp::Call) == 0,
          "repeated pure calls: wrapper calls remain after optimization");
    check(toyc::analyzeLoops(mainFunction).empty(),
          "repeated pure calls: exact hot loop was not collapsed");
    check(returnedConstant(mainFunction).has_value(),
          "repeated pure calls: final value was not folded");
}

void testEquivalentExactLoopShapes() {
    TestPipeline reversed(R"(
        int main() {
            int i = 0;
            int result = -1;
            while (10 > i) {
                result = i * 2 + 1;
                i = i + 3;
            }
            return result;
        }
    )", true);
    check(toyc::analyzeLoops(reversed.ir.functions[0]).empty(),
          "reversed comparison loop was not deleted");
    expectConstantReturn(reversed.ir.functions[0], 19,
                         "reversed comparison loop");

    TestPipeline inclusiveDescending(R"(
        int main() {
            int i = 10;
            int result = -1;
            while (i >= 1) {
                result = i;
                i = i - 3;
            }
            return result;
        }
    )", true);
    check(toyc::analyzeLoops(inclusiveDescending.ir.functions[0]).empty(),
          "inclusive descending loop was not deleted");
    expectConstantReturn(inclusiveDescending.ir.functions[0], 1,
                         "inclusive descending loop");

    TestPipeline falseEdgeBody(R"(
        int main() {
            int i = 0;
            int result = 7;
            while (!(i >= 8)) {
                result = i + 4;
                i = i + 2;
            }
            return result;
        }
    )", true);
    check(toyc::analyzeLoops(falseEdgeBody.ir.functions[0]).empty(),
          "inverted-condition loop was not deleted");
    expectConstantReturn(falseEdgeBody.ir.functions[0], 10,
                         "inverted-condition loop");
}

void testShapeChangesDoNotBreakSafetyBarriers() {
    TestPipeline effects(R"(
        int state = 1;
        int update(int x) {
            if (x < 0) state = state + 1;
            return state + x;
        }
        int main() {
            int i = 0;
            int result = 0;
            while (i < 8) {
                result = update(i);
                i = i + 1;
            }
            return result;
        }
    )", true);
    const auto& effectsMain = effects.ir.functions[1];
    check(countOp(effectsMain, toyc::IROp::StoreGlobal) != 0 ||
              countOp(effectsMain, toyc::IROp::Call) != 0,
          "safety barrier: conditional global write disappeared");

    TestPipeline trap(R"(
        int risky(int x, int divisor) { return x % divisor; }
        int main() {
            int i = 0;
            int result = 0;
            while (i < 8) {
                result = risky(i, i - 3);
                i = i + 1;
            }
            return result;
        }
    )", true);
    const auto& trapMain = trap.ir.functions[1];
    check(!toyc::analyzeLoops(trapMain).empty(),
          "safety barrier: potentially trapping loop was deleted");
}

} // namespace

int main() {
    try {
        testTransitivePureEffectsRemoveDeadHotCalls();
        testNestedCfgInliningAndCleanup();
        testRepeatedPureCallsThroughWrapper();
        testEquivalentExactLoopShapes();
        testShapeChangesDoNotBreakSafetyBarriers();
        std::cout << "optimization robustness tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
