#include "lexer.hpp"

#include <charconv>
#include <cctype>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace{

    bool isWordStart(char ch){
        const auto c = static_cast<unsigned char>(ch);
        return ch == '_' || std::isalpha(c);
    }

    bool isWordPart(char ch){
        const auto c = static_cast<unsigned char>(ch);
        return ch == '_' || std::isalnum(c);
    }

    bool isDigit(char ch){
        return std::isdigit(static_cast<unsigned char>(ch));
    }

    //区分关键字和标识符
    TokenType keyOrId(std::string_view text){
        static const std::unordered_map<std::string_view, TokenType> keywords{
            {"const", TokenType::Const},
            {"int", TokenType::Int},
            {"void", TokenType::Void},
            {"if", TokenType::If},
            {"else", TokenType::Else},
            {"while", TokenType::While},
            {"break", TokenType::Break},
            {"continue", TokenType::Continue},
            {"return", TokenType::Return},
        };

        const auto it = keywords.find(text);

        if (it != keywords.end())
            return it->second;

        return TokenType::Identifier;
    }

} // namespace

Lexer::Lexer(std::string source)
    : source(std::move(source)) {}

Token Lexer::next(){
    return scanToken();
}

bool Lexer::isAtEnd() const{
    return position >= source.size();
}

char Lexer::current() const{
    return isAtEnd() ? '\0' : source[position];
}

char Lexer::read(){
    if (isAtEnd())
        return '\0';

    const char ch = source[position++];

    if (ch == '\n'){
        ++line;
        column = 1;
    }
    else{
        ++column;
    }

    return ch;
}

bool Lexer::match(char expected){
    if (isAtEnd() || current() != expected){
        return false;
    }

    read();
    return true;
}

void Lexer::skip_whitespace(){
    while (!isAtEnd()){
        const char ch = current();

        if (std::isspace(static_cast<unsigned char>(ch))){
            read();
            continue;
        }

        // 单行注释
        if (ch == '/' &&
            position + 1 < source.size() &&
            source[position + 1] == '/'){
            read(); read();

            while (!isAtEnd() && current() != '\n')
                read();

            continue;
        }

        // 多行注释
        if (ch == '/' &&
            position + 1 < source.size() &&
            source[position + 1] == '*'){
            const SourceLocation location{line, column};

            read(); read();

            bool closed = false;

            while (!isAtEnd()){
                if (current() == '*' &&
                    position + 1 < source.size() &&
                    source[position + 1] == '/'){
                    read(); read();
                    closed = true;
                    break;
                }

                read();
            }

            if (!closed)
                lexerError(location, "unterminated block comment");

            continue;
        }

        break;
    }
}

Token Lexer::scanWord(SourceLocation location, std::size_t start){
    while (!isAtEnd() && isWordPart(current()))
        read();

    const std::string_view text{
        source.data() + start,
        position - start
    };

    Token token;
    token.type = keyOrId(text);
    token.lexeme = text;
    token.location = location;

    return token;
}

Token Lexer::scanNumber(SourceLocation location, std::size_t start){
    while (!isAtEnd() && isDigit(current()))
        read();

    const std::string_view text{
        source.data() + start,
        position - start
    };

    std::int64_t value = 0;
    const auto result = std::from_chars(
        text.data(),
        text.data() + text.size(),
        value
    );

    if (result.ec != std::errc{})
        lexerError(location, "integer literal is too large");

    Token token;
    token.type = TokenType::Number;
    token.lexeme = text;
    token.intValue = value;
    token.location = location;

    return token;
}

Token Lexer::scanToken(){
    skip_whitespace();

    const SourceLocation location{line, column};
    const std::size_t start = position;

    if (isAtEnd()){
        Token token;
        token.type = TokenType::End;
        token.lexeme = {};
        token.location = location;
        return token;
    }

    const char ch = read();

    //获得标识符或关键字token
    if (isWordStart(ch))
        return scanWord(location, start);

    //获得数字token
    if (isDigit(ch))
        return scanNumber(location, start);

    //获取操作符
    auto makeToken = [&](TokenType type){
        std::string_view text = {source.data() + start, position - start};
        Token token;
        token.type = type;
        token.lexeme = text;
        token.location = location;
        return token;
    };

    switch (ch){
    case '+':
        return makeToken(TokenType::Plus);
    case '-':
        return makeToken(TokenType::Minus);
    case '*':
        return makeToken(TokenType::Multiply);
    case '/':
        return makeToken(TokenType::Div);
    case '%':
        return makeToken(TokenType::Percent);
    case '(':
        return makeToken(TokenType::LParen);
    case ')':
        return makeToken(TokenType::RParen);
    case '{':
        return makeToken(TokenType::LBrace);
    case '}':
        return makeToken(TokenType::RBrace);
    case ',':
        return makeToken(TokenType::Comma);
    case ';':
        return makeToken(TokenType::Semicolon);
    case '=':
        if(match('='))
            return makeToken(TokenType::EqualEqual);
        return makeToken(TokenType::Assign);
    case '!':
        if (match('='))
            return makeToken(TokenType::EqualEqual);
        return makeToken(TokenType::Assign);

    case '<':
        if (match('='))
            return makeToken(TokenType::LessEqual);
        return makeToken(TokenType::Less);

    case '>':
        if (match('='))
            return makeToken(TokenType::GreaterEqual);
        return makeToken(TokenType::Greater);

    case '&':
        if (match('&'))
            return makeToken(TokenType::LogicalAnd);
        lexerError(location, "expected '&&'");

    case '|':
        if (match('|'))
            return makeToken(TokenType::LogicalOr);
        lexerError(location, "expected '||'");

    default:
        lexerError(location, "unexpected character '" + std::string(1, ch) + "'");
    }
}

void Lexer::lexerError(SourceLocation location, const std::string &message) const{
    throw std::runtime_error(
        std::to_string(location.line) + ":" +
        std::to_string(location.column) +
        ": lexical error: " + message);
}
