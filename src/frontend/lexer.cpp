#include "toyc/frontend/lexer.hpp"

#include <charconv>
#include <cctype>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

#include "toyc/support/diagnostic.hpp"

namespace toyc {
namespace {

bool isWordStart(char ch) {
    const auto value = static_cast<unsigned char>(ch);
    return ch == '_' || std::isalpha(value) != 0;
}

bool isWordPart(char ch) {
    const auto value = static_cast<unsigned char>(ch);
    return ch == '_' || std::isalnum(value) != 0;
}

bool isDigit(char ch) {
    return std::isdigit(static_cast<unsigned char>(ch)) != 0;
}

TokenType keywordOrIdentifier(std::string_view text) {
    static const std::unordered_map<std::string_view, TokenType> keywords{
        {"const", TokenType::Const}, {"int", TokenType::Int},
        {"void", TokenType::Void}, {"if", TokenType::If},
        {"else", TokenType::Else}, {"while", TokenType::While},
        {"break", TokenType::Break}, {"continue", TokenType::Continue},
        {"return", TokenType::Return},
    };
    const auto iterator = keywords.find(text);
    return iterator == keywords.end() ? TokenType::Identifier : iterator->second;
}

} // namespace

std::string_view tokenTypeName(TokenType type) noexcept {
    switch (type) {
#define TOYC_TOKEN_NAME(value) case TokenType::value: return #value
        TOYC_TOKEN_NAME(End); TOYC_TOKEN_NAME(Identifier); TOYC_TOKEN_NAME(Number);
        TOYC_TOKEN_NAME(Const); TOYC_TOKEN_NAME(Int); TOYC_TOKEN_NAME(Void);
        TOYC_TOKEN_NAME(If); TOYC_TOKEN_NAME(Else); TOYC_TOKEN_NAME(While);
        TOYC_TOKEN_NAME(Break); TOYC_TOKEN_NAME(Continue); TOYC_TOKEN_NAME(Return);
        TOYC_TOKEN_NAME(Plus); TOYC_TOKEN_NAME(Minus); TOYC_TOKEN_NAME(Multiply);
        TOYC_TOKEN_NAME(Div); TOYC_TOKEN_NAME(Percent); TOYC_TOKEN_NAME(Not);
        TOYC_TOKEN_NAME(Less); TOYC_TOKEN_NAME(Greater); TOYC_TOKEN_NAME(LessEqual);
        TOYC_TOKEN_NAME(GreaterEqual); TOYC_TOKEN_NAME(EqualEqual);
        TOYC_TOKEN_NAME(NotEqual); TOYC_TOKEN_NAME(LogicalAnd);
        TOYC_TOKEN_NAME(LogicalOr); TOYC_TOKEN_NAME(Assign);
        TOYC_TOKEN_NAME(LParen); TOYC_TOKEN_NAME(RParen); TOYC_TOKEN_NAME(LBrace);
        TOYC_TOKEN_NAME(RBrace); TOYC_TOKEN_NAME(Comma); TOYC_TOKEN_NAME(Semicolon);
#undef TOYC_TOKEN_NAME
    }
    return "Unknown";
}

Lexer::Lexer(std::string source) : source_(std::move(source)) {}
Token Lexer::next() { return scanToken(); }
bool Lexer::isAtEnd() const { return position_ >= source_.size(); }
char Lexer::current() const { return isAtEnd() ? '\0' : source_[position_]; }

char Lexer::read() {
    if (isAtEnd()) return '\0';
    const char ch = source_[position_++];
    if (ch == '\n') { ++line_; column_ = 1; } else { ++column_; }
    return ch;
}

bool Lexer::match(char expected) {
    if (isAtEnd() || current() != expected) return false;
    read();
    return true;
}

void Lexer::skipWhitespace() {
    while (!isAtEnd()) {
        if (std::isspace(static_cast<unsigned char>(current())) != 0) {
            read();
            continue;
        }
        if (current() == '/' && position_ + 1 < source_.size() && source_[position_ + 1] == '/') {
            read(); read();
            while (!isAtEnd() && current() != '\n') read();
            continue;
        }
        if (current() == '/' && position_ + 1 < source_.size() && source_[position_ + 1] == '*') {
            const SourceLocation location{line_, column_};
            read(); read();
            bool closed = false;
            while (!isAtEnd()) {
                if (current() == '*' && position_ + 1 < source_.size() && source_[position_ + 1] == '/') {
                    read(); read(); closed = true; break;
                }
                read();
            }
            if (!closed) lexerError(location, "unterminated block comment");
            continue;
        }
        break;
    }
}

Token Lexer::scanWord(SourceLocation location, std::size_t start) {
    while (!isAtEnd() && isWordPart(current())) read();
    const std::string_view text{source_.data() + start, position_ - start};
    return Token{keywordOrIdentifier(text), text, 0, location};
}

Token Lexer::scanNumber(SourceLocation location, std::size_t start) {
    while (!isAtEnd() && isDigit(current())) read();
    const std::string_view text{source_.data() + start, position_ - start};
    if (text.size() > 1 && text.front() == '0')
        lexerError(location, "leading zero is not allowed in decimal literal");
    std::int64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        lexerError(location, "integer literal is too large");
    return Token{TokenType::Number, text, value, location};
}

Token Lexer::scanToken() {
    skipWhitespace();
    const SourceLocation location{line_, column_};
    const std::size_t start = position_;
    if (isAtEnd()) return Token{TokenType::End, {}, 0, location};
    const char ch = read();
    if (isWordStart(ch)) return scanWord(location, start);
    if (isDigit(ch)) return scanNumber(location, start);
    const auto makeToken = [&](TokenType type) {
        return Token{type, std::string_view{source_.data() + start, position_ - start}, 0, location};
    };
    switch (ch) {
    case '+': return makeToken(TokenType::Plus);
    case '-': return makeToken(TokenType::Minus);
    case '*': return makeToken(TokenType::Multiply);
    case '/': return makeToken(TokenType::Div);
    case '%': return makeToken(TokenType::Percent);
    case '(': return makeToken(TokenType::LParen);
    case ')': return makeToken(TokenType::RParen);
    case '{': return makeToken(TokenType::LBrace);
    case '}': return makeToken(TokenType::RBrace);
    case ',': return makeToken(TokenType::Comma);
    case ';': return makeToken(TokenType::Semicolon);
    case '=': return makeToken(match('=') ? TokenType::EqualEqual : TokenType::Assign);
    case '!': return makeToken(match('=') ? TokenType::NotEqual : TokenType::Not);
    case '<': return makeToken(match('=') ? TokenType::LessEqual : TokenType::Less);
    case '>': return makeToken(match('=') ? TokenType::GreaterEqual : TokenType::Greater);
    case '&': if (match('&')) return makeToken(TokenType::LogicalAnd); lexerError(location, "expected '&&'");
    case '|': if (match('|')) return makeToken(TokenType::LogicalOr); lexerError(location, "expected '||'");
    default: lexerError(location, "unexpected character '" + std::string(1, ch) + "'");
    }
}

[[noreturn]] void Lexer::lexerError(SourceLocation location, const std::string& message) const {
    throw CompileError(location, "lexical", message);
}

} // namespace toyc
