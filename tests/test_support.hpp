#pragma once

#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

#include "toyc/frontend/ast.hpp"
#include "toyc/frontend/lexer.hpp"
#include "toyc/frontend/parser.hpp"
#include "toyc/frontend/semantic.hpp"
#include "toyc/ir/cfg_builder.hpp"
#include "toyc/ir/verifier.hpp"

inline void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct TestPipeline {
    toyc::DiagnosticEngine diagnostics;
    std::unique_ptr<toyc::CompUnit> ast;
    toyc::SemanticResult semantic;
    toyc::ModuleIR module;

    explicit TestPipeline(const std::string& source) {
        toyc::Lexer lexer(source);
        toyc::Parser parser(lexer, diagnostics);
        ast = parser.parseCompUnit();
        semantic = toyc::SemanticAnalyzer(diagnostics).analyze(*ast);
        module = toyc::CFGBuilder(semantic).build(*ast);
        toyc::verify(module, semantic);
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
