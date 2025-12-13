//
// Created by Funcieq on 27.11.2025.
//

#ifndef IGNIS_PARSER_H
#define IGNIS_PARSER_H
#include <vector>
#include <set>
#include "../token/token.h"
#include "../ast/node.h"
#include "../ast/arena.h"
#include <string>

struct ParseError
{
    std::string message;
    unsigned long pos;
    unsigned long line;
    unsigned long column;
    std::string tokenValue;
};

class Parser
{
public:
    std::vector<Token> tokens;
    unsigned long pos;
    NodeArena arena;
    std::string source;
    std::vector<ParseError> errors;
    bool lastConsumeSynthetic = false;
    std::set<std::string> definedStructs; // przechowuj nazwy zdefiniowanych struktur

    NodeArena Parse(std::vector<Token> *tokens, std::string_view src = "", const std::string &filePath = "");
    Token Peek(int offset = 0);
    Token Advance();
    Token Previous();
    bool IsAtEnd();
    Token Consume(TokenType expectedType, std::string errMsg);
    Node ParseStatement();
    Node ParseFunction();
    Node ParseStruct();
    IgnType ParseType();
    std::vector<Param> ParseParams();
    void ParseBody(NodeId funcId);
    Node ParseReturn();
    Node ParseLet();
    Node ParseIf();
    Node ParseWhile();
    Node ParseAssignment();
    Node ParseExpr();
    Node ParseTerm();
    Node ParseFactor();
    Node ParseLogic();
    void AddError(const std::string &msg, const Token &tok);
    void ReportErrors(const std::string &filePath = "");
    void Synchronize(TokenType expectedType);
};

#endif // IGNIS_PARSER_H