//
// Created by Funcieq on 29.11.2025.
//

#include "sema.h"
#include <iostream>

void SemanticAnalyzer::Analyze(NodeArena &arena, const std::string &source)
{
    std::cout << "Semantic Analysis started.\n";
    for (auto &node : arena.getAllNodes())
    {
        std::cout << "Sema: Analyzing node of type " << node.nodeType << "\n";
        if (node.nodeType == NodeType::TFun)
        {
            AnalyzeFunction(node, source);
        }
        else
        {
            std::cout << "Sema: Node type " << node.nodeType << " not handled yet.\n";
        }
    }
}

void SemanticAnalyzer::AnalyzeFunction(Node &funcNode, const std::string &source)
{
    if (funcNode.funcName == "main")
    {
        foundMain = true;
    }
    else
    {
        std::cout << "Analyzing function: " << funcNode.funcName << "\n";
    }
}

void SemanticAnalyzer::ReportError(const std::string &msg, const Node &node, const std::string &path)
{
    std::cerr << "Error: " << msg << " at " << node.funcName << " in " << path << " at line <?,?>" << "\n";
}