//
// Created by Funcieq on 29.11.2025.
//
#ifndef IGNIS_SEMA_H
#define IGNIS_SEMA_H
#include "../ast/arena.h"
#include "../ast/node.h"
#include <string>

class SemanticAnalyzer
{
public:
    SemanticAnalyzer() = default;
    void Analyze(NodeArena &arena, const std::string &source);

private:
    void AnalyzeFunction(Node &funcNode, const std::string &source);
    void ReportError(const std::string &msg, const Node &node, const std::string &source);
};
#endif // IGNIS_SEMA_H