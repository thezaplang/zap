//
// Created by Funcieq on 28.11.2025.
//

#include "parser.h"
#include "../../utils/utils.h"
#include <stdexcept>
#include <iostream>
#include <algorithm>

NodeArena Parser::Parse(std::vector<Token> *tokensInput, std::string_view src, const std::string &filePath)
{

    tokens = *tokensInput;
    pos = 0;
    arena = NodeArena();
    source = std::string(src);
    errors.clear();

    while (!IsAtEnd())
    {
        ParseStatement();
    }

    ReportErrors(filePath);

    return arena;
}

Node Parser::ParseStatement()
{
    if (Peek().type == TokenType::KFn)
    {
        Node func = ParseFunction();
        return func;
    }
    else
    {
        AddError(std::string("statement of type: ") + std::string(Peek().value) + " is not implemented yet", Peek());
        Advance();
        return Node();
    }
}

Node Parser::ParseFunction()
{
    Advance();
    Token nameTok = Consume(TokenType::Id, "Expected function name");
    std::string_view funcName = nameTok.value;

    Token lparenTok = Consume(TokenType::LParen, "Expected '('");
    std::vector<Param> params;
    if (!lastConsumeSynthetic)
    {
        params = ParseParams();
        Token rparenTok = Consume(TokenType::RParen, "Expected ')'");
        if (lastConsumeSynthetic)
        {

            Synchronize(TokenType::LBrace);
        }
    }
    else
    {
        // missing '(', synchronize to the function body start
        Synchronize(TokenType::LBrace);
    }
    Consume(TokenType::Arrow, "Expected '->' after function parameters");
    Token returnTypeTok = Consume(TokenType::Id, "Expected return type after '->'"); // TODO: parseType();
    Consume(TokenType::LBrace, "Expected '{'");

    Node funcNode;
    funcNode.nodeType = NodeType::TFun;
    funcNode.funcName = std::string(funcName);
    funcNode.paramList = params;
    IgnType rt;
    rt.isPtr = false;
    rt.isStruct = false;
    rt.isArray = false;
    rt.isRef = false;
    rt.base = stringToPrimType(std::string(returnTypeTok.value));
    funcNode.returnType = rt;

    NodeId funcId = arena.create(funcNode);

    ParseBody(funcId);

    Consume(TokenType::RBrace, "Expected '}'");

    return arena.get(funcId);
}

std::vector<Param> Parser::ParseParams()
{
    std::vector<Param> params;
    while (Peek().type != TokenType::RParen)
    {

        if (Peek().type == TokenType::LBrace || Peek().type == TokenType::EOF_TOKEN)
        {
            break;
        }

        std::string paramName = std::string(Consume(TokenType::Id, "Expected parameter name").value);
        Consume(TokenType::Colon, "Expected ':' after parameter name");
        std::string typeName = std::string(Consume(TokenType::Id, "Expected type name").value);

        IgnType paramType;
        paramType.isPtr = false;
        paramType.isStruct = false;
        paramType.isArray = false;
        paramType.isRef = false;
        paramType.base = stringToPrimType(typeName);

        Param param;
        param.name = paramName;
        param.type = paramType;

        params.push_back(param);

        if (Peek().type == TokenType::Comma)
        {
            Advance();
        }
        else
        {
            break;
        }
    }
    return params;
}

void Parser::ParseBody(NodeId funcId)
{
    while (!IsAtEnd() && Peek().type != TokenType::RBrace)
    {
        if (Peek().type == TokenType::KReturn)
        {
            Node returnNode = ParseReturn();
            NodeId childId = arena.create(returnNode);
            arena.get(funcId).body.push_back(childId);
        }
        else if (Peek().type == TokenType::Id)
        {
            if (Peek(1).type == TokenType::LParen)
            {
                Node exprNode = ParseExpr();
                NodeId exprId = arena.create(exprNode);
                arena.get(funcId).body.push_back(exprId);
                // Optional semicolon after function call expressions
                if (Peek().type == TokenType::Semi)
                    Advance();
            }
            else
            {
                AddError("Only function call expressions are supported as statements starting with identifier", Peek());
                Advance();
            }
        }
        else
        {
            AddError(std::string("Statement type not yet supported in body: ") + std::string(Peek().value), Peek());
            // synchronize to next statement boundary to avoid cascading errors
            Synchronize(TokenType::RBrace);
            if (Peek().type == TokenType::Semi)
                Advance();
        }
    }
}

Node Parser::ParseReturn()
{
    Consume(TokenType::KReturn, "Expected 'return'");

    Node returnNode;
    returnNode.nodeType = NodeType::TRet;

    Node returnValue = ParseExpr();
    if (returnValue.nodeType != NodeType::TExpr)
    {
        AddError("Expected return value expression", Peek());
    }

    NodeId exprId = arena.create(returnValue);
    returnNode.body.push_back(exprId);

    // Store type info from the expression
    returnNode.exprType = returnValue.exprType;

    Consume(TokenType::Semi, "Expected ';' after return statement");

    return returnNode;
}

Node Parser::ParseExpr()
{
    Node exprNode;
    exprNode.nodeType = NodeType::TExpr;
    exprNode.exprType.isPtr = false;
    exprNode.exprType.isStruct = false;
    exprNode.exprType.isArray = false;
    exprNode.exprType.isRef = false;

    Token current = Peek();

    switch (current.type)
    {
    case TokenType::ConstInt:
    {
        Token tok = Consume(TokenType::ConstInt, "Expected integer literal in expression");
        exprNode.exprType.base = PrimType::PTI32;
        exprNode.exprKind = ExprType::ExprInt;
        exprNode.intValue = std::stoi(std::string(tok.value));
        break;
    }

    case TokenType::ConstString:
    {
        Token tok = Consume(TokenType::ConstString, "Expected string literal in expression");
        exprNode.exprType.base = PrimType::PTString;
        exprNode.exprKind = ExprType::ExprString;
        exprNode.stringValue = std::string(tok.value);
        break;
    }
    case TokenType::Id:
    {
        Token funcNameTok = Consume(TokenType::Id, "Expected identifier");

        if (Peek().type == TokenType::LParen)
        {
            exprNode.exprKind = ExprType::ExprFuncCall;
            exprNode.funcName = std::string(funcNameTok.value);
            exprNode.exprType.base = PrimType::PTI32; // Placeholder

            Consume(TokenType::LParen, "Expected '('");

            while (Peek().type != TokenType::RParen && !IsAtEnd())
            {
                Node argNode = ParseExpr();
                NodeId argId = arena.create(argNode);
                exprNode.exprArgs.push_back(argId);

                if (Peek().type == TokenType::Comma)
                {
                    Advance();
                }
                else
                {
                    break;
                }
            }

            Consume(TokenType::RParen, "Expected ')' after function arguments");
        }
        else
        {
            // Just an identifier (variable reference)
            exprNode.exprKind = ExprType::ExprInt;
            exprNode.funcName = std::string(funcNameTok.value);
            exprNode.exprType.base = PrimType::PTI32; // TODO: Look up actual type
        }

        break;
    }

    default:
        AddError("Unsupported expression starting with token: " + std::string(current.value), current);
        Advance();
        break;
    }

    return exprNode;
}

Token Parser::Peek(int offset)
{
    if (IsAtEnd())
    {
        return tokens.back();
    }
    return tokens[pos + offset];
}

Token Parser::Advance()
{
    if (!IsAtEnd())
        pos++;
    return Previous();
}

Token Parser::Previous()
{
    return tokens[pos - 1];
}

bool Parser::IsAtEnd()
{
    return pos >= tokens.size() || tokens[pos].type == TokenType::EOF_TOKEN;
}

Token Parser::Consume(TokenType expectedType, std::string errMsg)
{
    if (Peek().type == expectedType)
    {
        lastConsumeSynthetic = false;
        return Advance();
    }
    AddError(errMsg + " Found: " + std::string(Peek().value), Peek());

    // Attempt to synchronize: advance until we find the expected token or a safe recovery token
    while (!IsAtEnd())
    {
        TokenType t = Peek().type;
        if (t == expectedType)
        {
            return Advance();
        }

        if (t == TokenType::Semi || t == TokenType::RBrace || t == TokenType::RParen || t == TokenType::Comma || t == TokenType::EOF_TOKEN || (expectedType == TokenType::RParen && t == TokenType::LBrace))
        {
            // stop synchronizing here; return a synthetic token for the expected type
            break;
        }
        Advance();
    }

    unsigned long curPos = pos < tokens.size() ? tokens[pos].pos : (unsigned long)source.size();
    std::string_view emptyView = curPos <= source.size() ? std::string_view(source.data() + curPos, 0) : std::string_view();
    lastConsumeSynthetic = true;
    return Token(expectedType, curPos, emptyView);
}

void Parser::Synchronize(TokenType expectedType)
{

    while (!IsAtEnd() && Peek().type != expectedType)
    {
        TokenType t = Peek().type;
        if (t == TokenType::Semi || t == TokenType::RBrace || t == TokenType::RParen || t == TokenType::EOF_TOKEN)
        {
            return;
        }
        Advance();
    }
}

void Parser::AddError(const std::string &msg, const Token &tok)
{
    ParseError e;
    e.message = msg;
    e.pos = tok.pos;
    e.tokenValue = std::string(tok.value);

    unsigned long line = 1;
    unsigned long column = 1;
    unsigned long maxPos = std::min<unsigned long>(tok.pos, source.size());
    for (unsigned long i = 0; i < maxPos; ++i)
    {
        if (source[i] == '\n')
        {
            ++line;
            column = 1;
        }
        else
        {
            ++column;
        }
    }
    e.line = line;
    e.column = column;

    errors.push_back(e);
}

void Parser::ReportErrors(const std::string &filePath)
{
    if (errors.empty())
        return;
    std::cerr << "Found " << errors.size() << " parse error(s):\n";
    for (auto &e : errors)
    {
        std::cerr << "Error: " << e.message;
        std::cerr << " in " << filePath << " at line " << e.line << ", column " << e.column;
        if (!e.tokenValue.empty())
            std::cerr << " (token: '" << e.tokenValue << "')";
        std::cerr << "\n";
    }
}
