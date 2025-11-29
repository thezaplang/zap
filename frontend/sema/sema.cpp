//
// Created by Funcieq on 29.11.2025.
//

#include "sema.h"
#include <iostream>

void SemanticAnalyzer::Analyze(NodeArena &arena, const std::string &source)
{
    for (auto &node : arena.getAllNodes())
    {
        if (node.nodeType == TFun)
        {
            AnalyzeFunction(node, source);
        }
    }
}

void SemanticAnalyzer::AnalyzeFunction(Node &funcNode, const std::string &source)
{
    // Analyze function parameters
    for (const auto &param : funcNode.paramList)
    {
        // Check parameter types
        if (param.type.isPtr)
        {
            // Report pointer type
            ReportError("Pointer type not allowed", funcNode, source);
        }
    }
}

void SemanticAnalyzer::ReportError(const std::string &msg, const Node &node, const std::string &path)
{
    std::cerr << "Error: " << msg << " at " << node.funcName << " in " << path << " at line <?,?>" << "\n";
}