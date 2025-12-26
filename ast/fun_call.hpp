#pragma once
#include "nodes.hpp"

class FunCall : public ExpressionNode, public StatementNode {
public:
    std::string funcName_;
    std::vector<std::unique_ptr<ExpressionNode>> arguments_;
};