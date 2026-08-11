#pragma once
#include "token.hpp"
#include <string>

class Lexer {
private:
    std::string source;           //源代码
    std::size_t position = 0;     //当前读取位置
    std::size_t line = 1;         //当前字符所在行号
    std::size_t column = 1;       //当前字符所在列号

public:
    explicit Lexer(std::string source);

    Token next();

private:
    Token scanToken();

    void skip_whitespace();
    
    char current() const;
    char read();
    bool match(char expected);
    bool isAtEnd() const;

    Token scanWord(SourceLocation location, std::size_t start);
    Token scanNumber(SourceLocation loction, std::size_t start);

    [[noreturn]] void lexerError(SourceLocation location, const std::string& message) const;
};
