#include <string>
#include <vector>

#include "test_support.hpp"
#include "toyc/frontend/lexer.hpp"

int main() {
    using toyc::TokenType;
    toyc::Lexer lexer("const int void if else while break continue return _x 0 + - * / % = == ! != < <= > >= && || ( ) { } , ;");
    const std::vector<TokenType> expected{
        TokenType::Const, TokenType::Int, TokenType::Void, TokenType::If,
        TokenType::Else, TokenType::While, TokenType::Break, TokenType::Continue,
        TokenType::Return, TokenType::Identifier, TokenType::Number,
        TokenType::Plus, TokenType::Minus, TokenType::Multiply, TokenType::Div,
        TokenType::Percent, TokenType::Assign, TokenType::EqualEqual,
        TokenType::Not, TokenType::NotEqual, TokenType::Less, TokenType::LessEqual,
        TokenType::Greater, TokenType::GreaterEqual, TokenType::LogicalAnd,
        TokenType::LogicalOr, TokenType::LParen, TokenType::RParen,
        TokenType::LBrace, TokenType::RBrace, TokenType::Comma,
        TokenType::Semicolon, TokenType::End};
    for (const auto type : expected) check(lexer.next().type == type, "wrong token sequence");
    check(lexer.next().type == TokenType::End, "End token is not stable");

    toyc::Lexer comments("/*a\nb*/\nint");
    const auto keyword = comments.next();
    check(keyword.type == TokenType::Int && keyword.location.line == 3 && keyword.location.column == 1,
          "comment location tracking failed");
    expectCompileError([] { toyc::Lexer bad("012"); (void)bad.next(); }, "1:1: lexical error");
    expectCompileError([] { toyc::Lexer bad("&"); (void)bad.next(); }, "expected '&&'");
    expectCompileError([] { toyc::Lexer bad("|"); (void)bad.next(); }, "expected '||'");
    expectCompileError([] { toyc::Lexer bad("\n @"); (void)bad.next(); }, "2:2: lexical error");
    expectCompileError([] { toyc::Lexer bad("/*"); (void)bad.next(); }, "unterminated block comment");
    expectCompileError([] { toyc::Lexer bad("999999999999999999999999"); (void)bad.next(); }, "integer literal is too large");
}
