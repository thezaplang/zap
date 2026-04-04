#pragma once
#include "expr_node.hpp"
#include "visitor.hpp"
#include <memory>

class TernaryExpr : public ExpressionNode {
public:
  std::unique_ptr<ExpressionNode> condition_;
  std::unique_ptr<ExpressionNode> thenExpr_;
  std::unique_ptr<ExpressionNode> elseExpr_;

  TernaryExpr(std::unique_ptr<ExpressionNode> cond,
              std::unique_ptr<ExpressionNode> thenE,
              std::unique_ptr<ExpressionNode> elseE)
      : condition_(std::move(cond)), thenExpr_(std::move(thenE)), elseExpr_(std::move(elseE)) {}

  void accept(Visitor &v) override { v.visit(*this); }
};
