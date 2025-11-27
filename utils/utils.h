//
// Created by Funcieq on 26.11.2025.
//

#ifndef IGNIS_UTILS_H
#define IGNIS_UTILS_H
#include <string>
bool isAlpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool isKeyword(std::string_view keyword) {
    if (keyword == "fn") {
        return true;
    }else if (keyword == "return") {
        return true;
    }else {
        return false;
    }
}

bool isOperator(char op) {
    if (op == '+' || op == '-' || op == '*' || op == '/' || op == '%') {
        return true;
    }
    else {
        return false;
    }
}

TokenType getTokenType(std::string_view token) {
    if (token == "fn") {
        return TokenType::KFn;
    }else if (token == "return") {
        return TokenType::KReturn;
    }else {
        //TODO: add to error list
        exit(1);
    }
}
#endif //IGNIS_UTILS_H