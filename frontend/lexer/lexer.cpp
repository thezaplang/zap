//
// Created by Funcieq on 26.11.2025.
//

#include "lexer.h"
#include "../../utils/utils.h"
#include <cstdlib>
#include <iostream>
#include <string>

std::vector<Token> Lexer::Tokenize(std::string_view content) {
    std::vector<Token> tokens;

    current_file = std::string(content);
    pos = 0;

    while (true) {
        char c = Peek();
        if (c == '\0') break;  
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            Advance();
            continue;
        }

        // ;
        if (c == ';') {
            tokens.emplace_back(TokenType::Semi, pos, std::string_view(current_file.data() + pos, 1));
            Advance();
            continue;
        }

        // (
        if (c == '(') {
            tokens.emplace_back(TokenType::LParen, pos, std::string_view(current_file.data() + pos, 1));
            Advance();
            continue;
        }

        // )
        if (c == ')') {
            tokens.emplace_back(TokenType::RParen, pos, std::string_view(current_file.data() + pos, 1));
            Advance();
            continue;
        }

        // {
        if (c == '{') {
            tokens.emplace_back(TokenType::LBrace, pos, std::string_view(current_file.data() + pos, 1));
            Advance();
            continue;
        }

        // }
        if (c == '}') {
            tokens.emplace_back(TokenType::RBrace, pos, std::string_view(current_file.data() + pos, 1));
            Advance();
            continue;
        }

        // IDENTYFIKATOR / KEYWORD
        if (isAlpha(c)) {
            size_t start = pos;
            while (isAlpha(Peek()))
                Advance();


            std::string_view id(current_file.data() + start, pos - start);

            if (isKeyword(id)) {
                tokens.emplace_back(getTokenType(id), start, id);
            } else {
                tokens.emplace_back(TokenType::Id, start, id);
            }
            continue;
        }

        if (pos >= current_file.size()) {
            break;
        }

    =
        std::cerr << "Unknown character: " << c << " at pos " << pos << "\n";
        Advance();
    }

    return tokens;
}

char Lexer::Peek() {
    if (pos < current_file.size()) return current_file[pos];
    return '\0';
}

char Lexer::Consume() {
    char c = Peek();
    if (c != '\0') pos++;
    return c;
}

void Lexer::Advance() {
    if (pos < current_file.size()) {
        pos++;
    } else {
        std::cerr << "cannot advance!\n";
        exit(1);
    }
}
