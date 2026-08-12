#include <exception>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

#include "toyc/backend/asm_printer.hpp"
#include "toyc/backend/frame.hpp"
#include "toyc/backend/isel.hpp"
#include "toyc/backend/mir_printer.hpp"
#include "toyc/backend/mir_verifier.hpp"
#include "toyc/backend/phi_lowering.hpp"
#include "toyc/backend/regalloc.hpp"
#include "toyc/frontend/ast_printer.hpp"
#include "toyc/frontend/lexer.hpp"
#include "toyc/frontend/parser.hpp"
#include "toyc/frontend/semantic.hpp"
#include "toyc/ir/cfg_builder.hpp"
#include "toyc/ir/cfg_utils.hpp"
#include "toyc/ir/ir_printer.hpp"
#include "toyc/ir/ssa_builder.hpp"
#include "toyc/ir/verifier.hpp"
#include "toyc/opt/pipeline.hpp"
#include "toyc/support/diagnostic.hpp"

namespace {

struct CompilerOptions {
    bool optimize = false;
    bool verifyEach = false;
    bool dumpTokens = false;
    bool dumpAst = false;
    bool dumpCfg = false;
    bool dumpIr = false;
    bool dumpIrBeforeOpt = false;
    bool dumpIrAfterEach = false;
    bool printPassStats = false;
    bool dumpMir = false;
    bool dumpMirAfterRA = false;
};

CompilerOptions parseOptions(int argc, char** argv) {
    CompilerOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "-opt") options.optimize = true;
        else if (argument == "--verify-each") options.verifyEach = true;
        else if (argument == "--dump-tokens") options.dumpTokens = true;
        else if (argument == "--dump-ast") options.dumpAst = true;
        else if (argument == "--dump-cfg") options.dumpCfg = true;
        else if (argument == "--dump-ir") options.dumpIr = true;
        else if (argument == "--dump-ir-before-opt") options.dumpIrBeforeOpt = true;
        else if (argument == "--dump-ir-after-each") options.dumpIrAfterEach = true;
        else if (argument == "--print-pass-stats") options.printPassStats = true;
        else if (argument == "--dump-mir") options.dumpMir = true;
        else if (argument == "--dump-mir-after-ra") options.dumpMirAfterRA = true;
        else throw toyc::CompileError({}, "command line", "unknown option '" + std::string(argument) + "'");
    }
    return options;
}

void dumpTokens(const std::string& source) {
    toyc::Lexer lexer(source);
    while (true) {
        const toyc::Token token = lexer.next();
        std::cerr << token.location.line << ':' << token.location.column << ' '
                  << toyc::tokenTypeName(token.type);
        if (!token.lexeme.empty()) std::cerr << " '" << token.lexeme << '\'';
        if (token.type == toyc::TokenType::Number) std::cerr << " = " << token.intValue;
        std::cerr << '\n';
        if (token.type == toyc::TokenType::End) break;
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        const CompilerOptions options = parseOptions(argc, argv);
        bool verifyIntermediate = options.verifyEach;
#ifndef NDEBUG
        verifyIntermediate = true;
#endif
        const std::string source{std::istreambuf_iterator<char>(std::cin),
                                 std::istreambuf_iterator<char>()};
        if (options.dumpTokens) dumpTokens(source);
        toyc::DiagnosticEngine diagnostics;
        toyc::Lexer lexer(source);
        toyc::Parser parser(lexer, diagnostics);
        auto ast = parser.parseCompUnit();
        toyc::SemanticAnalyzer analyzer(diagnostics);
        auto semantic = analyzer.analyze(*ast);
        auto cfg = toyc::CFGBuilder(semantic).build(*ast);
        toyc::removeUnreachable(cfg);
        toyc::verifyCFG(cfg, semantic);
        if (options.dumpAst) toyc::printAst(std::cerr, *ast);
        if (options.dumpCfg) toyc::printCFG(std::cerr, cfg, semantic);
        auto ir = toyc::SSABuilder(semantic).build(cfg);
        if (verifyIntermediate) toyc::verifyIR(ir, semantic);
        toyc::OptimizationOptions optimizationOptions;
        optimizationOptions.enabled = options.optimize;
        optimizationOptions.verifyEach = verifyIntermediate;
        optimizationOptions.dumpBefore = options.dumpIrBeforeOpt;
        optimizationOptions.dumpAfterEach = options.dumpIrAfterEach;
        optimizationOptions.printStats = options.printPassStats;
        toyc::runOptimizationPipeline(ir, semantic, optimizationOptions, std::cerr);
        if (verifyIntermediate) toyc::verifyIR(ir, semantic);
        if (options.dumpIr) toyc::printIR(std::cerr, ir, semantic);
        toyc::ISelOptions iselOptions;
        iselOptions.fuseCompareBranches = options.optimize;
        auto machine = toyc::InstructionSelector(semantic, iselOptions).lower(ir);
        toyc::resolveParallelCopies(machine);
        if (verifyIntermediate) toyc::verifyMIR(machine, toyc::MIRStage::PreRegisterAllocation);
        if (options.dumpMir) toyc::printMIR(std::cerr, machine, semantic);
        for (auto& function : machine.functions) {
            toyc::RegAllocOptions registerOptions;
            registerOptions.enableCopyHints = options.optimize;
            registerOptions.enableRematerialization = options.optimize;
            toyc::LinearScanRegisterAllocator(registerOptions).run(function);
            if (verifyIntermediate) toyc::verifyMIR(function, toyc::MIRStage::PostRegisterAllocation);
        }
        if (options.dumpMirAfterRA) toyc::printMIR(std::cerr, machine, semantic);
        for (auto& function : machine.functions) {
            toyc::FrameLowering().run(function);
            toyc::verifyMIR(function, toyc::MIRStage::AfterFrameLowering);
        }
        toyc::AsmPrinter(std::cout).print(machine, semantic);
        return 0;
    } catch (const toyc::CompileError& error) {
        std::cerr << error.what() << '\n';
        return error.category().find("verification") != std::string::npos ? 2 : 1;
    } catch (const std::exception& error) {
        std::cerr << "internal error: " << error.what() << '\n';
        return 2;
    }
}
