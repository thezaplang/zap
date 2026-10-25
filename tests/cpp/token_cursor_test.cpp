#include "lexer/lexer.hpp"
#include "parser/token_cursor.hpp"
#include "utils/diagnostics.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

Token makeToken(TokenType type, const std::string& value, size_t offset) {
    return Token(
        type,
        value,
        SourceSpan(1, offset + 1, offset, value.size(), "token_cursor_test.zp")
    );
}

void testEmptyCursor() {
    const std::vector<Token> tokens;
    zap::TokenCursor cursor(tokens);

    require(cursor.isAtEnd(), "empty cursor did not start at end");
    require(
        cursor.peek().type == TokenType::EOF_TOKEN,
        "empty cursor did not return EOF from peek"
    );
    require(
        cursor.previous().type == TokenType::EOF_TOKEN,
        "empty cursor did not return EOF from previous"
    );

    cursor.advance(3);
    require(cursor.isAtEnd(), "advancing an empty cursor moved it from end");
}

void testRangeAndEndSpan() {
    const std::vector<Token> tokens = {
        makeToken(TokenType::ID, "first", 0),
        makeToken(TokenType::PLUS, "+", 6),
        makeToken(TokenType::ID, "last", 8),
    };
    zap::TokenCursor cursor(tokens, 1, 2);

    require(
        cursor.begin() == 1 && cursor.end() == 2,
        "cursor did not preserve its requested range"
    );
    require(cursor.position() == 1, "cursor did not start at range begin");
    require(cursor.peek().type == TokenType::PLUS, "cursor returned a token outside its range");
    require(
        cursor.peek(1).type == TokenType::EOF_TOKEN,
        "cursor did not stop lookahead at range end"
    );

    cursor.advance();
    require(cursor.isAtEnd(), "cursor did not reach range end");
    require(
        cursor.previous().type == TokenType::PLUS,
        "cursor did not retain the previous in-range token"
    );
    require(cursor.peek().type == TokenType::EOF_TOKEN, "cursor did not return EOF at range end");
    require(
        cursor.peek().span.offset == 8 && cursor.peek().span.length == 0,
        "EOF span did not start at the next token after the range"
    );

    cursor.advance(10);
    require(cursor.position() == cursor.end(), "cursor advanced beyond its range end");
}

void testClampedRange() {
    const std::vector<Token> tokens = {
        makeToken(TokenType::ID, "only", 0),
    };
    zap::TokenCursor cursor(tokens, 8, 20);

    require(
        cursor.begin() == 1 && cursor.end() == 1,
        "cursor did not clamp an out-of-bounds range"
    );
    require(cursor.isAtEnd(), "clamped empty range did not start at end");
    require(cursor.peek().type == TokenType::EOF_TOKEN, "clamped empty range did not return EOF");
}

void testMultilineFinalTokenEndSpan() {
    const std::string source = "\"first\\nline\nlast\"";
    zap::DiagnosticEngine diagnostics(source);
    Lexer lexer(diagnostics);
    const auto tokens = lexer.tokenize(source);
    zap::TokenCursor cursor(tokens);

    cursor.advance();
    const Token& end = cursor.peek();
    require(
        end.type == TokenType::EOF_TOKEN,
        "cursor did not reach EOF after final multiline token"
    );
    require(end.span.offset == source.size(), "EOF offset did not follow final multiline token");
    require(
        end.span.line == 0 && end.span.column == 0,
        "EOF invented a line and column after a multiline token"
    );
}

} // namespace

int main() {
    testEmptyCursor();
    testRangeAndEndSpan();
    testClampedRange();
    testMultilineFinalTokenEndSpan();
    return 0;
}
