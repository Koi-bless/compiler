#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace toyc {

enum class TokenType {
    End, Identifier, Number,
    Const, Int, Void, If, Else, While, Break, Continue, Return,
    Plus, Minus, Multiply, Div, Percent, Not,
    Less, Greater, LessEqual, GreaterEqual, EqualEqual, NotEqual,
    LogicalAnd, LogicalOr, Assign,
    LParen, RParen, LBrace, RBrace, Comma, Semicolon
};

struct SourceLocation {
    std::size_t line = 1;
    std::size_t column = 1;
    friend bool operator==(const SourceLocation&, const SourceLocation&) = default;
};

struct SourceRange {
    SourceLocation begin{};
    SourceLocation end{};
};

struct Token {
    TokenType type = TokenType::End;
    std::string_view lexeme{};
    std::int64_t intValue = 0;
    SourceLocation location{};
};

[[nodiscard]] std::string_view tokenTypeName(TokenType type) noexcept;

} // namespace toyc
