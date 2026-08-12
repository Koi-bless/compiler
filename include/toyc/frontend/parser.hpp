#pragma once

#include <memory>
#include <string_view>

#include "toyc/frontend/ast.hpp"
#include "toyc/frontend/lexer.hpp"
#include "toyc/support/diagnostic.hpp"

namespace toyc {

class Parser {
public:
    Parser(Lexer& lexer, DiagnosticEngine& diagnostics);
    std::unique_ptr<CompUnit> parseCompUnit();

private:
    Lexer& lexer_;
    DiagnosticEngine& diagnostics_;
    Token current_;
    Token next_;

    void advance();
    bool consume(TokenType type);
    Token expect(TokenType type, std::string_view message);
    TopLevelPtr parseTopLevel();
    std::unique_ptr<Declaration> parseDeclaration();
    std::unique_ptr<Declaration> parseDeclarationTail(bool isConst,
                                                      SourceLocation start,
                                                      Token name);
    std::unique_ptr<FunctionDecl> parseFunctionTail(ValueType returnType,
                                                    SourceLocation start,
                                                    Token name);
    std::unique_ptr<BlockStmt> parseBlock();
    StmtPtr parseStmt();
    ExprPtr parseExpr();
    ExprPtr parseLogicalOr();
    ExprPtr parseLogicalAnd();
    ExprPtr parseEquality();
    ExprPtr parseRelational();
    ExprPtr parseAdditive();
    ExprPtr parseMultiplicative();
    ExprPtr parseUnary();
    ExprPtr parsePrimary();
};

} // namespace toyc
