#pragma once
#include "expr_node.hpp"
#include <memory>

class UnaryExpr : public ExpressionNode{
public:
    std::string op_;
    std::unique_ptr<ExpressionNode> expr_;
    UnaryExpr() = default;
    UnaryExpr(std::string op, std::unique_ptr<ExpressionNode> expr)
    :
    op_(op),
    expr_(std::move(expr)){}
};