#include "toyc/opt/pipeline.hpp"

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

    {
        const PassResult result = propagateImmutableGlobals(module, semantic);
#ifndef NDEBUG
        verifyIR(module, semantic);
#else
        if (options.verifyEach) verifyIR(module, semantic);
#endif
        if (options.printStats)
            diagnostics << "pass ImmutableGlobals: changed="
                        << (result.changed ? 1 : 0)
                        << ", inst_replaced=" << result.instructionsReplaced
                        << '\n';
        if (options.dumpAfterEach) {
            diagnostics << "*** IR after ImmutableGlobals: changed="
                        << (result.changed ? 1 : 0) << " ***\n";
            printIR(diagnostics, module, semantic);
        }
    }

    run("LocalDAG", [](IRFunction& function) { return runLocalDAG(function); });
    run("InstCombine", [](IRFunction& function) { return runInstCombine(function); });
    for (unsigned iteration = 1; iteration <= options.maxFixpointIterations; ++iteration) {
        bool changed = false;
        changed = run("SCCP", [](IRFunction& function) { return runSCCP(function); },
                      iteration).changed || changed;
        changed = run("SimplifyCFG", [](IRFunction& function) {
            return runSimplifyCFG(function);
        }, iteration).changed || changed;
        changed = run("InstCombine", [](IRFunction& function) {
            return runInstCombine(function);
        }, iteration).changed || changed;
        changed = run("DCE", [](IRFunction& function) {
            return runDCE(function, true);
        }, iteration).changed || changed;
        if (!changed) break;
    }
    run("TailRecursionElimination", [](IRFunction& function) {
        return runTailRecursionElimination(function);
    });
    auto effects = analyzeFunctionEffects(module);
    run("FunctionEffectsDCE", [&](IRFunction& function) {
        return runDCE(function, true, &effects);
    });
    run("PureCallGVN", [&](IRFunction& function) {
        return runGVN(function, &effects);
    });
    run("FunctionEffectsDCE", [&](IRFunction& function) {
        return runDCE(function, true, &effects);
    });
    {
        const PassResult result = runFunctionInlining(module);
        for (auto& function : module.functions) canonicalizeIR(function);
#ifndef NDEBUG
        verifyIR(module, semantic);
#else
        if (options.verifyEach) verifyIR(module, semantic);
#endif
        if (options.printStats)
            diagnostics << "pass Inline: changed=" << (result.changed ? 1 : 0)
                        << ", inst_removed=" << result.instructionsRemoved
                        << ", inst_replaced=" << result.instructionsReplaced
                        << ", blocks_removed=" << result.blocksRemoved << '\n';
        if (options.dumpAfterEach) {
            diagnostics << "*** IR after Inline: changed="
                        << (result.changed ? 1 : 0) << " ***\n";
            printIR(diagnostics, module, semantic);
        }
    }
    effects = analyzeFunctionEffects(module);
    run("LoopFinalValue/LoopDeletion", [&](IRFunction& function) {
        return runLoopFinalValueAndDeletion(function, module);
    });
    for (unsigned iteration = 1; iteration <= options.maxFixpointIterations;
         ++iteration) {
        bool changed = false;
        changed = run("SCCP", [](IRFunction& function) { return runSCCP(function); },
                      iteration).changed || changed;
        changed = run("InstCombine", [](IRFunction& function) {
            return runInstCombine(function);
        }, iteration).changed || changed;
        changed = run("SimplifyCFG", [](IRFunction& function) {
            return runSimplifyCFG(function);
        }, iteration).changed || changed;
        changed = run("GVN", [&](IRFunction& function) {
            return runGVN(function, &effects);
        }, iteration).changed || changed;
        changed = run("DCE", [&](IRFunction& function) {
            return runDCE(function, true, &effects);
        }, iteration).changed || changed;
        if (!changed) break;
    }
    effects = analyzeFunctionEffects(module);
    run("GVN", [&](IRFunction& function) { return runGVN(function, &effects); });
    run("DCE", [&](IRFunction& function) {
        return runDCE(function, true, &effects);
    });
    run("LICM", [](IRFunction& function) { return runLICM(function); });
    run("GVN", [&](IRFunction& function) { return runGVN(function, &effects); });
    run("DCE", [&](IRFunction& function) {
        return runDCE(function, true, &effects);
    });
    run("SimplifyCFG", [](IRFunction& function) { return runSimplifyCFG(function); });
    run("CanonicalizeIR", [](IRFunction& function) {
        return PassResult{canonicalizeIR(function)};
    });
}

} // namespace toyc
