#pragma once

#include <cstddef>
#include <string>

#include "toyc/frontend/token.hpp"

namespace toyc {

class Lexer {
public:
    explicit Lexer(std::string source);
    Token next();

private:
    std::string source_;
    std::size_t position_ = 0;
    std::size_t line_ = 1;
    std::size_t column_ = 1;

    Token scanToken();
    void skipWhitespace();
    [[nodiscard]] char current() const;
    char read();
    bool match(char expected);
    [[nodiscard]] bool isAtEnd() const;
    Token scanWord(SourceLocation location, std::size_t start);
    Token scanNumber(SourceLocation location, std::size_t start);
    [[noreturn]] void lexerError(SourceLocation location,
                                 const std::string& message) const;
};

} // namespace toyc
