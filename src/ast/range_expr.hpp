#pragma once
#include "expr_node.hpp"
#include "visitor.hpp"
#include <memory>

class RangeExpr : public ExpressionNode {
public:
    std::unique_ptr<ExpressionNode> start_;
    std::unique_ptr<ExpressionNode> end_;
    std::unique_ptr<ExpressionNode> step_;

    RangeExpr(std::unique_ptr<ExpressionNode> start,
              std::unique_ptr<ExpressionNode> end,
              std::unique_ptr<ExpressionNode> step = nullptr)
        : start_(std::move(start)), end_(std::move(end)),
          step_(std::move(step)) {}

    void accept(Visitor &v) override { v.visit(*this); }
};