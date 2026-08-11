#include <sstream>
#include <string>

#include "test_support.hpp"
#include "toyc/frontend/ast_printer.hpp"

int main() {
    toyc::DiagnosticEngine diagnostics;
    toyc::Lexer lexer("int main(){int x=1+2*3; if(x) if(0) x=1; else x=2; return x;}");
    toyc::Parser parser(lexer, diagnostics);
    auto ast = parser.parseCompUnit();
    std::ostringstream output;
    toyc::printAst(output, *ast);
    check(output.str().find("(add 1 (mul 2 3))") != std::string::npos, "operator precedence is wrong");
    check(output.str().find("(if x\n      (if 0") != std::string::npos, "dangling else shape is wrong");
    expectCompileError([] {
        toyc::DiagnosticEngine d; toyc::Lexer l("int main( { return 0; }");
        (void)toyc::Parser(l, d).parseCompUnit();
    }, "syntax error");
}
