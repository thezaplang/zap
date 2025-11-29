//
// Created by Funcieq on 29.11.2025.
//

#include "sema.h"
#include <iostream>

void SemanticAnalyzer::Analyze(NodeArena &arena, const std::string &source, const std::string &filePath)
{

    currentFilePath = filePath;
    std::cout << "Semantic Analysis started.\n";

    for (auto &node : arena.getAllNodes())
    {
        if (node.nodeType != NodeType::TFun)
            continue;
        //  std::cout << "Sema: Analyzing node of type " << node.nodeType << "\n";
        AnalyzeFunction(node, arena, source);
    }
}

void SemanticAnalyzer::AnalyzeFunction(Node &funcNode, NodeArena &arena, const std::string &source)
{
    if (funcNode.funcName == "main")
    {
        foundMain = true;
    }
    else
    {
        std::cout << "Registering function: " << funcNode.funcName << "\n";
    }
    SetCurrentReturnType(funcNode.returnType);
    AnalyzeFunctionBody(funcNode, arena, source);
    RegisterFunction({funcNode.funcName, funcNode.returnType, funcNode.paramList});
}

void SemanticAnalyzer::AnalyzeFunctionBody(Node &funcNode, NodeArena &arena, const std::string &source)
{
    for (auto &childId : funcNode.body)
    {
        Node &childNode = arena.get(childId);
        if (childNode.nodeType == NodeType::TRet)
        {
            AnalyzeReturnStatement(childNode, source);
        }
        else
        {
            std::cout << "Sema: Function body node type " << childNode.nodeType << " not handled yet.\n";
        }
    }
}

void SemanticAnalyzer::AnalyzeReturnStatement(Node &returnNode, const std::string &source)
{
    std::cout << "Analyzing return statement.\n";
    IgnType expectedType = GetCurrentReturnType();
    if (returnNode.exprType.base != expectedType.base)
    {
        ReportError("Return type mismatch", returnNode, source);
    }
}

void SemanticAnalyzer::ReportError(const std::string &msg, const Node &node, const std::string &source)
{

    size_t pos = std::string::npos;
    if (!node.funcName.empty())
    {
        pos = source.find(node.funcName);
    }
    if (pos == std::string::npos && node.nodeType == NodeType::TRet)
    {
        // look for a 'return' followed by the literal value if available
        size_t p = source.find("return");
        if (p != std::string::npos)
        {
            if (node.intValue != 0 || source.find("0", p) != std::string::npos)
            {
                std::string num = std::to_string(node.intValue);
                size_t p2 = source.find(num, p);
                if (p2 != std::string::npos)
                    pos = p2;
                else
                    pos = p;
            }
            else
            {
                pos = p;
            }
        }
    }
    if (pos == std::string::npos)
        pos = 0;

    unsigned long line = 1;
    unsigned long column = 1;
    unsigned long maxPos = static_cast<unsigned long>(std::min(pos, source.size()));
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

    std::cerr << "Error: " << msg;
    if (!node.funcName.empty())
        std::cerr << " at function '" << node.funcName << "'";
    std::cerr << " in " << currentFilePath << " at line " << line << ", column " << column << "\n";
}

void SemanticAnalyzer::RegisterFunction(const FunctionSymbol &funcSym)
{
    functionSymbols.push_back(funcSym);
}
bool SemanticAnalyzer::FuncExsistsCheck(const std::string &funcName)
{
    for (const auto &func : functionSymbols)
    {
        if (func.name == funcName)
        {
            return true;
        }
    }
    return false;
}

void SemanticAnalyzer::SetCurrentReturnType(const IgnType &retType)
{
    currentReturnType = retType;
}