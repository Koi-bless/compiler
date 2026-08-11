#pragma once

#include <cstdint>
#include <string_view>

enum class TokenType{
    End,

    Identifier,
    Number,

    //关键字
    Const,
    Int,
    Void,
    If,
    Else,
    While,
    Break,
    Continue,
    Return,

    //运算符
    Plus,          // +
    Minus,         // -
    Multiply,      // *
    Div,           // /
    Percent,       // %
    Not,           // !

    Less,          // <
    Greater,       // >
    LessEqual,     // <=
    GreaterEqual,  // >=
    EqualEqual,    // ==
    NotEqual,      // !=
    LogicalAnd,     // &&
    LogicalOr,     // ||

    Assign,        // =

    //分隔符
    LParen,        // (
    RParen,        // )
    LBrace,        // {
    RBrace,        // }
    Comma,         // ,
    Semicolon      // ;
};

struct SourceLocation{
    std::size_t line = 1;
    std::size_t column = 1;
};

struct Token{
    TokenType type;
    std::string_view lexeme;
    std::int64_t intValue = 0;
    SourceLocation location;
};
