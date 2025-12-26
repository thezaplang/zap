#pragma once
#include "statement_node.hpp"
#include "expr_node.hpp"

class AssignNode : public StatementNode {
public:
    std::string target_;
    std::unique_ptr<ExpressionNode> expr_;
    AssignNode() = default;

    AssignNode(std::string target, 
        std::unique_ptr<ExpressionNode> expr)
        :
        target_(target),
        expr_(std::move(expr)){}
};