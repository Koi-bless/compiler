#pragma once

#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

#include "toyc/backend/asm_printer.hpp"
#include "toyc/backend/frame.hpp"
#include "toyc/backend/isel.hpp"
#include "toyc/backend/mir_verifier.hpp"
#include "toyc/backend/phi_lowering.hpp"
#include "toyc/backend/regalloc.hpp"
#include "toyc/frontend/lexer.hpp"
#include "toyc/frontend/parser.hpp"
#include "toyc/frontend/semantic.hpp"
#include "toyc/ir/cfg_builder.hpp"
#include "toyc/ir/cfg_utils.hpp"
#include "toyc/ir/ssa_builder.hpp"
#include "toyc/ir/verifier.hpp"
#include "toyc/opt/pipeline.hpp"

inline void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct TestPipeline {
    toyc::DiagnosticEngine diagnostics;
    std::unique_ptr<toyc::CompUnit> ast;
    toyc::SemanticResult semantic;
    toyc::CFGModule cfg;
    toyc::IRModule ir;
    toyc::MachineModule machine;
    bool optimized = false;

    explicit TestPipeline(const std::string& source, bool optimize = false)
        : optimized(optimize) {
        toyc::Lexer lexer(source);
        toyc::Parser parser(lexer, diagnostics);
        ast = parser.parseCompUnit();
        semantic = toyc::SemanticAnalyzer(diagnostics).analyze(*ast);
        cfg = toyc::CFGBuilder(semantic).build(*ast);
        toyc::removeUnreachable(cfg);
        toyc::verifyCFG(cfg, semantic);
        ir = toyc::SSABuilder(semantic).build(cfg);
        toyc::verifyIR(ir, semantic);
        toyc::OptimizationOptions options;
        options.enabled = optimize;
        options.verifyEach = true;
        std::ostringstream diagnosticsOutput;
        toyc::runOptimizationPipeline(ir, semantic, options, diagnosticsOutput);
        machine = toyc::InstructionSelector(
            semantic, toyc::ISelOptions{optimize}).lower(ir);
        toyc::resolveParallelCopies(machine);
        toyc::verifyMIR(machine, toyc::MIRStage::PreRegisterAllocation);
    }

    toyc::MachineModule buildFinalMIR() const {
        auto result = machine;
        for (auto& function : result.functions) {
            toyc::LinearScanRegisterAllocator(
                toyc::RegAllocOptions{optimized, optimized}).run(function);
            toyc::verifyMIR(function, toyc::MIRStage::PostRegisterAllocation);
            toyc::FrameLowering().run(function);
            toyc::verifyMIR(function, toyc::MIRStage::AfterFrameLowering);
        }
        return result;
    }

    std::string emitAssembly() const {
        auto final = buildFinalMIR();
        std::ostringstream output;
        toyc::AsmPrinter(output).print(final, semantic);
        return output.str();
    }
};

template <class Action>
void expectCompileError(Action action, const std::string& text) {
    try { action(); }
    catch (const toyc::CompileError& error) {
        check(std::string(error.what()).find(text) != std::string::npos,
              "unexpected diagnostic: " + std::string(error.what()));
        return;
    }
    throw std::runtime_error("expected CompileError");
}
