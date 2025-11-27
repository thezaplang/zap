//
// Created by Funcieq on 26.11.2025.
//

#ifndef IGNIS_TOKEN_H
#define IGNIS_TOKEN_H
#include <string_view>

enum TokenType {
    KFn,
    KReturn,
    KLet,
    Id,
    LParen,
    RParen,
    LBrace,
    RBrace,
    Semi,
    Colon,
    ConstInt,
    Operator,
    Assign,
};

struct Token {
    TokenType type;
    unsigned long pos;
    std::string_view value;

    Token(TokenType t, unsigned long pos, std::string_view val) {
        this->type = t;
        this->pos = pos;
        this->value = val;
    }
};
#endif //IGNIS_TOKEN_H