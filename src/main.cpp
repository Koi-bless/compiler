#include <exception>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

#include "toyc/backend/asm_printer.hpp"
#include "toyc/frontend/ast_printer.hpp"
#include "toyc/frontend/lexer.hpp"
#include "toyc/frontend/parser.hpp"
#include "toyc/frontend/semantic.hpp"
#include "toyc/ir/cfg_builder.hpp"
#include "toyc/ir/verifier.hpp"
#include "toyc/support/diagnostic.hpp"

namespace {

struct CompilerOptions {
    bool dumpTokens = false;
    bool dumpAst = false;
    bool dumpCfg = false;
};

CompilerOptions parseOptions(int argc, char** argv) {
    CompilerOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "-opt" || argument == "--verify-each") continue;
        if (argument == "--dump-tokens") options.dumpTokens = true;
        else if (argument == "--dump-ast") options.dumpAst = true;
        else if (argument == "--dump-cfg") options.dumpCfg = true;
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
        const std::string source{std::istreambuf_iterator<char>(std::cin),
                                 std::istreambuf_iterator<char>()};
        if (options.dumpTokens) dumpTokens(source);
        toyc::DiagnosticEngine diagnostics;
        toyc::Lexer lexer(source);
        toyc::Parser parser(lexer, diagnostics);
        auto ast = parser.parseCompUnit();
        toyc::SemanticAnalyzer analyzer(diagnostics);
        auto semantic = analyzer.analyze(*ast);
        auto module = toyc::CFGBuilder(semantic).build(*ast);
        toyc::verify(module, semantic);
        if (options.dumpAst) toyc::printAst(std::cerr, *ast);
        if (options.dumpCfg) toyc::printCfg(std::cerr, module, semantic);
        toyc::AsmPrinter(std::cout).print(module, semantic);
        return 0;
    } catch (const toyc::CompileError& error) {
        std::cerr << error.what() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "internal error: " << error.what() << '\n';
        return 2;
    }
}
