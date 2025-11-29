//
// Created by Funcieq on 26.11.2025.
//

#ifndef IGNIS_UTILS_H
#define IGNIS_UTILS_H
#include <string>
#include <string_view>
#include "../frontend/ast/node.h"

inline bool isAlpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

inline bool isKeyword(std::string_view keyword)
{
    if (keyword == "fn")
    {
        return true;
    }
    else if (keyword == "return")
    {
        return true;
    }
    else
    {
        return false;
    }
}

inline bool isOperator(char op)
{
    if (op == '+' || op == '-' || op == '*' || op == '/' || op == '%')
    {
        return true;
    }
    else
    {
        return false;
    }
}

inline TokenType getTokenType(std::string_view token)
{
    if (token == "fn")
    {
        return TokenType::KFn;
    }
    else if (token == "return")
    {
        return TokenType::KReturn;
    }
    else
    {
        // TODO: add to error list
        exit(1);
    }
}

inline PrimType stringToPrimType(const std::string &typeStr)
{
    if (typeStr == "i32")
    {
        return PrimType::PTI32;
    }
    else if (typeStr == "f32")
    {
        return PrimType::PTF32;
    }
    else if (typeStr == "bool")
    {
        return PrimType::PTBool;
    }
    else if (typeStr == "char")
    {
        return PrimType::PTChar;
    }
    else if (typeStr == "void")
    {
        return PrimType::PTVoid;
    }
    else
    {
        return PrimType::PTUserType;
    }
}
#endif // IGNIS_UTILS_H