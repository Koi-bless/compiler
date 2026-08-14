#include "toyc/opt/pipeline.hpp"

#include <algorithm>
#include <functional>
#include <ostream>
#include <string>

#include "toyc/ir/ir_printer.hpp"
#include "toyc/ir/verifier.hpp"
#include "toyc/opt/dce.hpp"
#include "toyc/opt/function_effects.hpp"
#include "toyc/opt/gvn.hpp"
#include "toyc/opt/inline.hpp"
#include "toyc/opt/instcombine.hpp"
#include "toyc/opt/ir_utils.hpp"
#include "toyc/opt/licm.hpp"
#include "toyc/opt/local_dag.hpp"
#include "toyc/opt/loop_transform.hpp"
#include "toyc/opt/sccp.hpp"
#include "toyc/opt/simplify_cfg.hpp"
#include "toyc/opt/tail_recursion.hpp"

namespace toyc {

void runOptimizationPipeline(IRModule& module, const SemanticResult& semantic,
                             const OptimizationOptions& options,
                             std::ostream& diagnostics) {
    if (options.dumpBefore) {
        diagnostics << "*** IR before optimization ***\n";
        printIR(diagnostics, module, semantic);
    }
    const auto run = [&](const std::string& name,
                         const std::function<PassResult(IRFunction&)>& pass,
                         unsigned iteration = 0) {
        PassResult result;
        for (auto& function : module.functions) {
            result += pass(function);
            result.changed = canonicalizeIR(function) || result.changed;
        }
#ifndef NDEBUG
        verifyIR(module, semantic);
#else
        if (options.verifyEach) verifyIR(module, semantic);
#endif
        if (options.printStats) {
            diagnostics << "pass " << name;
            if (iteration != 0) diagnostics << " iteration=" << iteration;
            diagnostics << ": changed=" << (result.changed ? 1 : 0)
                        << ", inst_removed=" << result.instructionsRemoved
                        << ", inst_replaced=" << result.instructionsReplaced
                        << ", blocks_removed=" << result.blocksRemoved << '\n';
        }
        if (options.dumpAfterEach) {
            diagnostics << "*** IR after " << name;
            if (iteration != 0) diagnostics << " (iteration " << iteration << ')';
            diagnostics << ": changed=" << (result.changed ? 1 : 0)
                        << ", inst_removed=" << result.instructionsRemoved
                        << ", blocks_removed=" << result.blocksRemoved << " ***\n";
            printIR(diagnostics, module, semantic);
        }
        return result;
    };
    const auto runModule = [&](const std::string& name,
                               const std::function<PassResult()>& pass,
                               unsigned iteration = 0) {
        PassResult result = pass();
        for (auto& function : module.functions)
            result.changed = canonicalizeIR(function) || result.changed;
#ifndef NDEBUG
        verifyIR(module, semantic);
#else
        if (options.verifyEach) verifyIR(module, semantic);
#endif
        if (options.printStats) {
            diagnostics << "pass " << name;
            if (iteration != 0) diagnostics << " iteration=" << iteration;
            diagnostics << ": changed=" << (result.changed ? 1 : 0)
                        << ", inst_removed=" << result.instructionsRemoved
                        << ", inst_replaced=" << result.instructionsReplaced
                        << ", blocks_removed=" << result.blocksRemoved << '\n';
        }
        if (options.dumpAfterEach) {
            diagnostics << "*** IR after " << name;
            if (iteration != 0) diagnostics << " (iteration " << iteration << ')';
            diagnostics << ": changed=" << (result.changed ? 1 : 0)
                        << ", inst_removed=" << result.instructionsRemoved
                        << ", blocks_removed=" << result.blocksRemoved << " ***\n";
            printIR(diagnostics, module, semantic);
        }
        return result;
    };

    run("CanonicalizeIR", [](IRFunction& function) {
        return PassResult{canonicalizeIR(function)};
    });
    run("TrivialPhiElimination", [](IRFunction& function) {
        return PassResult{eliminateTrivialPhis(function)};
    });
    if (!options.enabled) {
        run("DCE", [](IRFunction& function) { return runDCE(function, true); });
        return;
    }

    runModule("ImmutableGlobals", [&] {
        return propagateImmutableGlobals(module, semantic);
    });

    run("LocalDAG", [](IRFunction& function) { return runLocalDAG(function); });
    run("InstCombine", [](IRFunction& function) { return runInstCombine(function); });
    const auto cleanup = [&](const FunctionEffectAnalysis* effects) {
        PassResult total;
        for (unsigned iteration = 1;
             iteration <= options.maxFixpointIterations; ++iteration) {
            PassResult current;
            current += run("SCCP", [](IRFunction& function) {
                return runSCCP(function);
            }, iteration);
            current += run("SimplifyCFG", [](IRFunction& function) {
                return runSimplifyCFG(function);
            }, iteration);
            current += run("InstCombine", [](IRFunction& function) {
                return runInstCombine(function);
            }, iteration);
            if (effects)
                current += run("GVN", [&](IRFunction& function) {
                    return runGVN(function, effects);
                }, iteration);
            current += run("DCE", [&](IRFunction& function) {
                return runDCE(function, true, effects);
            }, iteration);
            total += current;
            if (!current.changed) break;
        }
        return total;
    };
    cleanup(nullptr);

    run("TailRecursionElimination", [](IRFunction& function) {
        return runTailRecursionElimination(function);
    });

    constexpr std::size_t inlineInstructionLimit = 192;
    std::size_t inlineBudget = 2048;
    for (unsigned round = 1; round <= options.maxFixpointIterations; ++round) {
        PassResult roundResult;

        auto effects = analyzeFunctionEffects(module);
        roundResult += run("FunctionEffectsDCE", [&](IRFunction& function) {
            return runDCE(function, true, &effects);
        }, round);
        roundResult += run("PureCallGVN", [&](IRFunction& function) {
            return runGVN(function, &effects);
        }, round);
        roundResult += run("FunctionEffectsDCE", [&](IRFunction& function) {
            return runDCE(function, true, &effects);
        }, round);

        if (inlineBudget != 0) {
            const PassResult inlined = runModule("Inline", [&] {
                return runFunctionInlining(
                    module, inlineBudget, inlineInstructionLimit);
            }, round);
            roundResult += inlined;
            inlineBudget -= std::min(inlineBudget, inlined.instructionsReplaced);
        }

        roundResult += cleanup(nullptr);
        effects = analyzeFunctionEffects(module);
        roundResult += run("FunctionEffectsDCE", [&](IRFunction& function) {
            return runDCE(function, true, &effects);
        }, round);
        roundResult += run("GVN", [&](IRFunction& function) {
            return runGVN(function, &effects);
        }, round);
        roundResult += run("FunctionEffectsDCE", [&](IRFunction& function) {
            return runDCE(function, true, &effects);
        }, round);

        roundResult += run("LoopFinalValue/LoopDeletion",
                           [&](IRFunction& function) {
            return runLoopFinalValueAndDeletion(function, module);
        }, round);

        effects = analyzeFunctionEffects(module);
        roundResult += cleanup(&effects);
        roundResult += run("LICM", [](IRFunction& function) {
            return runLICM(function);
        }, round);
        effects = analyzeFunctionEffects(module);
        roundResult += cleanup(&effects);

        roundResult += runModule("ImmutableGlobals", [&] {
            return propagateImmutableGlobals(module, semantic);
        }, round);
        effects = analyzeFunctionEffects(module);
        roundResult += cleanup(&effects);

        if (!roundResult.changed) break;
    }

    run("CanonicalizeIR", [](IRFunction& function) {
        return PassResult{canonicalizeIR(function)};
    });
}

} // namespace toyc
