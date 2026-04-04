#pragma once
#include "expr_node.hpp"
#include "visitor.hpp"
#include <memory>

class TernaryExpr : public ExpressionNode {
public:
  std::unique_ptr<ExpressionNode> condition_;
  std::unique_ptr<ExpressionNode> thenExpr_;
  std::unique_ptr<ExpressionNode> elseExpr_;

  TernaryExpr(std::unique_ptr<ExpressionNode> condition,
              std::unique_ptr<ExpressionNode> thenExpr,
              std::unique_ptr<ExpressionNode> elseExpr)
      : condition_(std::move(condition)), thenExpr_(std::move(thenExpr)),
        elseExpr_(std::move(elseExpr)) {}

  void accept(Visitor &v) override { v.visit(*this); }
};
