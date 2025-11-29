//
// Created by Funcieq on 29.11.2025.
//
#ifndef IGNIS_SEMA_H
#define IGNIS_SEMA_H
#include "../ast/arena.h"
#include "../ast/node.h"
#include <string>

struct FunctionSymbol
{
    std::string name;
    IgnType returnType;
    std::vector<Param> parameters;
};

class SemanticAnalyzer
{
public:
    bool foundMain = false;
    SemanticAnalyzer() = default;
    void Analyze(NodeArena &arena, const std::string &source, const std::string &filePath);
    void RegisterFunction(const FunctionSymbol &funcSym);
    bool FuncExsistsCheck(const std::string &funcName);
    void SetCurrentReturnType(const IgnType &retType);
    IgnType GetCurrentReturnType() const { return currentReturnType; }

private:
    void AnalyzeFunction(Node &funcNode, NodeArena &arena, const std::string &source);
    void AnalyzeFunctionBody(Node &funcNode, NodeArena &arena, const std::string &source);
    void AnalyzeReturnStatement(Node &returnNode, const std::string &source);
    void ReportError(const std::string &msg, const Node &node, const std::string &source);
    std::vector<FunctionSymbol> functionSymbols;
    IgnType currentReturnType;
    std::string currentFilePath;
};
#endif // IGNIS_SEMA_H