#include "toyc/frontend/parser.hpp"

#include <string>
#include <utility>

namespace toyc {
namespace {

SourceRange at(SourceLocation begin, SourceLocation end) { return SourceRange{begin, end}; }

BinaryOp binaryOperator(TokenType type) {
    switch (type) {
    case TokenType::Plus: return BinaryOp::Add;
    case TokenType::Minus: return BinaryOp::Sub;
    case TokenType::Multiply: return BinaryOp::Mul;
    case TokenType::Div: return BinaryOp::Div;
    case TokenType::Percent: return BinaryOp::Rem;
    case TokenType::Less: return BinaryOp::Less;
    case TokenType::Greater: return BinaryOp::Greater;
    case TokenType::LessEqual: return BinaryOp::LessEqual;
    case TokenType::GreaterEqual: return BinaryOp::GreaterEqual;
    case TokenType::EqualEqual: return BinaryOp::Equal;
    case TokenType::NotEqual: return BinaryOp::NotEqual;
    case TokenType::LogicalAnd: return BinaryOp::LogicalAnd;
    case TokenType::LogicalOr: return BinaryOp::LogicalOr;
    default: throw CompileError({}, "internal", "token is not a binary operator");
    }
}

} // namespace

Parser::Parser(Lexer& lexer, DiagnosticEngine& diagnostics)
    : lexer_(lexer), diagnostics_(diagnostics), current_(lexer_.next()), next_(lexer_.next()) {}

void Parser::advance() { current_ = next_; next_ = lexer_.next(); }

bool Parser::consume(TokenType type) {
    if (current_.type != type) return false;
    advance();
    return true;
}

Token Parser::expect(TokenType type, std::string_view message) {
    if (current_.type != type) {
        diagnostics_.fail(current_.location, "syntax",
                          std::string(message) + ", got " +
                              std::string(tokenTypeName(current_.type)));
    }
    const Token result = current_;
    advance();
    return result;
}

std::unique_ptr<CompUnit> Parser::parseCompUnit() {
    const SourceLocation begin = current_.location;
    auto unit = std::make_unique<CompUnit>(at(begin, begin));
    while (current_.type != TokenType::End) unit->items.push_back(parseTopLevel());
    if (unit->items.empty()) diagnostics_.fail(current_.location, "syntax", "compilation unit must not be empty");
    unit->range.end = current_.location;
    return unit;
}

TopLevelPtr Parser::parseTopLevel() {
    const SourceLocation start = current_.location;
    if (current_.type == TokenType::Const) return parseDeclaration();
    ValueType type;
    if (consume(TokenType::Int)) type = ValueType::Int;
    else if (consume(TokenType::Void)) type = ValueType::Void;
    else diagnostics_.fail(current_.location, "syntax", "expected top-level declaration or function");
    const Token name = expect(TokenType::Identifier, "expected identifier");
    if (current_.type == TokenType::LParen) return parseFunctionTail(type, start, name);
    if (type == ValueType::Void) diagnostics_.fail(current_.location, "syntax", "void is only valid as a function return type");
    return parseDeclarationTail(false, start, name);
}

std::unique_ptr<Declaration> Parser::parseDeclaration() {
    const SourceLocation start = current_.location;
    const bool isConst = consume(TokenType::Const);
    expect(TokenType::Int, "expected 'int' in declaration");
    const Token name = expect(TokenType::Identifier, "expected declaration name");
    return parseDeclarationTail(isConst, start, name);
}

std::unique_ptr<Declaration> Parser::parseDeclarationTail(bool isConst,
                                                          SourceLocation start,
                                                          Token name) {
    expect(TokenType::Assign, "expected '=' in declaration");
    auto init = parseExpr();
    const Token semicolon = expect(TokenType::Semicolon, "expected ';' after declaration");
    return std::make_unique<Declaration>(at(start, semicolon.location), isConst,
                                         std::string(name.lexeme), std::move(init));
}

std::unique_ptr<FunctionDecl> Parser::parseFunctionTail(ValueType returnType,
                                                        SourceLocation start,
                                                        Token name) {
    expect(TokenType::LParen, "expected '('");
    std::vector<ParamDecl> params;
    if (current_.type != TokenType::RParen) {
        do {
            const Token type = expect(TokenType::Int, "expected 'int' before parameter");
            const Token parameter = expect(TokenType::Identifier, "expected parameter name");
            params.emplace_back(at(type.location, parameter.location), std::string(parameter.lexeme));
        } while (consume(TokenType::Comma));
    }
    expect(TokenType::RParen, "expected ')' after parameters");
    auto body = parseBlock();
    const SourceLocation end = body->range.end;
    return std::make_unique<FunctionDecl>(at(start, end), returnType,
                                          std::string(name.lexeme), std::move(params),
                                          std::move(body));
}

std::unique_ptr<BlockStmt> Parser::parseBlock() {
    const Token left = expect(TokenType::LBrace, "expected '{'");
    auto block = std::make_unique<BlockStmt>(at(left.location, left.location));
    while (current_.type != TokenType::RBrace) {
        if (current_.type == TokenType::End) diagnostics_.fail(current_.location, "syntax", "expected '}' before end of input");
        block->items.push_back(parseStmt());
    }
    const Token right = expect(TokenType::RBrace, "expected '}'");
    block->range.end = right.location;
    return block;
}

StmtPtr Parser::parseStmt() {
    const SourceLocation start = current_.location;
    if (current_.type == TokenType::LBrace) return parseBlock();
    if (consume(TokenType::Semicolon)) return std::make_unique<EmptyStmt>(at(start, start));
    if (current_.type == TokenType::Const || current_.type == TokenType::Int) {
        auto declaration = parseDeclaration();
        const SourceRange range = declaration->range;
        return std::make_unique<DeclStmt>(range, std::move(declaration));
    }
    if (consume(TokenType::If)) {
        expect(TokenType::LParen, "expected '(' after 'if'");
        auto condition = parseExpr();
        expect(TokenType::RParen, "expected ')' after condition");
        auto thenStmt = parseStmt();
        StmtPtr elseStmt;
        if (consume(TokenType::Else)) elseStmt = parseStmt();
        const SourceLocation end = elseStmt ? elseStmt->range.end : thenStmt->range.end;
        return std::make_unique<IfStmt>(at(start, end), std::move(condition),
                                        std::move(thenStmt), std::move(elseStmt));
    }
    if (consume(TokenType::While)) {
        expect(TokenType::LParen, "expected '(' after 'while'");
        auto condition = parseExpr();
        expect(TokenType::RParen, "expected ')' after condition");
        auto body = parseStmt();
        const SourceLocation end = body->range.end;
        return std::make_unique<WhileStmt>(at(start, end), std::move(condition), std::move(body));
    }
    if (consume(TokenType::Break)) {
        const Token end = expect(TokenType::Semicolon, "expected ';' after 'break'");
        return std::make_unique<BreakStmt>(at(start, end.location));
    }
    if (consume(TokenType::Continue)) {
        const Token end = expect(TokenType::Semicolon, "expected ';' after 'continue'");
        return std::make_unique<ContinueStmt>(at(start, end.location));
    }
    if (consume(TokenType::Return)) {
        ExprPtr value;
        if (current_.type != TokenType::Semicolon) value = parseExpr();
        const Token end = expect(TokenType::Semicolon, "expected ';' after 'return'");
        return std::make_unique<ReturnStmt>(at(start, end.location), std::move(value));
    }
    if (current_.type == TokenType::Identifier && next_.type == TokenType::Assign) {
        const Token name = current_;
        advance(); advance();
        auto value = parseExpr();
        const Token end = expect(TokenType::Semicolon, "expected ';' after assignment");
        return std::make_unique<AssignStmt>(at(start, end.location), std::string(name.lexeme), std::move(value));
    }
    auto expression = parseExpr();
    const Token end = expect(TokenType::Semicolon, "expected ';' after expression");
    return std::make_unique<ExprStmt>(at(start, end.location), std::move(expression));
}

ExprPtr Parser::parseExpr() { return parseLogicalOr(); }

ExprPtr Parser::parseLogicalOr() {
    auto lhs = parseLogicalAnd();
    while (current_.type == TokenType::LogicalOr) {
        const auto start = lhs->range.begin; const auto op = current_.type; advance();
        auto rhs = parseLogicalAnd(); const auto end = rhs->range.end;
        lhs = std::make_unique<BinaryExpr>(at(start, end), binaryOperator(op), std::move(lhs), std::move(rhs));
    }
    return lhs;
}

ExprPtr Parser::parseLogicalAnd() {
    auto lhs = parseRelational();
    while (current_.type == TokenType::LogicalAnd) {
        const auto start = lhs->range.begin; const auto op = current_.type; advance();
        auto rhs = parseRelational(); const auto end = rhs->range.end;
        lhs = std::make_unique<BinaryExpr>(at(start, end), binaryOperator(op), std::move(lhs), std::move(rhs));
    }
    return lhs;
}

ExprPtr Parser::parseRelational() {
    auto lhs = parseAdditive();
    while (current_.type == TokenType::Less || current_.type == TokenType::Greater ||
           current_.type == TokenType::LessEqual || current_.type == TokenType::GreaterEqual ||
           current_.type == TokenType::EqualEqual || current_.type == TokenType::NotEqual) {
        const auto start = lhs->range.begin; const auto op = current_.type; advance();
        auto rhs = parseAdditive(); const auto end = rhs->range.end;
        lhs = std::make_unique<BinaryExpr>(at(start, end), binaryOperator(op), std::move(lhs), std::move(rhs));
    }
    return lhs;
}

ExprPtr Parser::parseAdditive() {
    auto lhs = parseMultiplicative();
    while (current_.type == TokenType::Plus || current_.type == TokenType::Minus) {
        const auto start = lhs->range.begin; const auto op = current_.type; advance();
        auto rhs = parseMultiplicative(); const auto end = rhs->range.end;
        lhs = std::make_unique<BinaryExpr>(at(start, end), binaryOperator(op), std::move(lhs), std::move(rhs));
    }
    return lhs;
}

ExprPtr Parser::parseMultiplicative() {
    auto lhs = parseUnary();
    while (current_.type == TokenType::Multiply || current_.type == TokenType::Div || current_.type == TokenType::Percent) {
        const auto start = lhs->range.begin; const auto op = current_.type; advance();
        auto rhs = parseUnary(); const auto end = rhs->range.end;
        lhs = std::make_unique<BinaryExpr>(at(start, end), binaryOperator(op), std::move(lhs), std::move(rhs));
    }
    return lhs;
}

ExprPtr Parser::parseUnary() {
    if (current_.type == TokenType::Plus || current_.type == TokenType::Minus || current_.type == TokenType::Not) {
        const Token op = current_; advance();
        auto operand = parseUnary(); const auto end = operand->range.end;
        const UnaryOp unary = op.type == TokenType::Plus ? UnaryOp::Plus :
                              op.type == TokenType::Minus ? UnaryOp::Negate : UnaryOp::LogicalNot;
        return std::make_unique<UnaryExpr>(at(op.location, end), unary, std::move(operand));
    }
    return parsePrimary();
}

ExprPtr Parser::parsePrimary() {
    if (current_.type == TokenType::Number) {
        const Token number = current_; advance();
        return std::make_unique<IntegerExpr>(at(number.location, number.location), number.intValue);
    }
    if (current_.type == TokenType::Identifier) {
        const Token name = current_; advance();
        if (!consume(TokenType::LParen))
            return std::make_unique<NameExpr>(at(name.location, name.location), std::string(name.lexeme));
        std::vector<ExprPtr> arguments;
        if (current_.type != TokenType::RParen) {
            do { arguments.push_back(parseExpr()); } while (consume(TokenType::Comma));
        }
        const Token right = expect(TokenType::RParen, "expected ')' after arguments");
        return std::make_unique<CallExpr>(at(name.location, right.location), std::string(name.lexeme), std::move(arguments));
    }
    if (consume(TokenType::LParen)) {
        auto expression = parseExpr();
        expect(TokenType::RParen, "expected ')' after expression");
        return expression;
    }
    diagnostics_.fail(current_.location, "syntax", "expected expression");
}

} // namespace toyc
