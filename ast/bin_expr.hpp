#pragma once
#include "expr_node.hpp"
#include "visitor.hpp"
#include <memory>
#include <string>

class BinExpr : public ExpressionNode {
public:
  std::unique_ptr<ExpressionNode> left_;
  std::string op_;
  std::unique_ptr<ExpressionNode> right_;
  BinExpr() = default;
  BinExpr(std::unique_ptr<ExpressionNode> left, std::string op,
          std::unique_ptr<ExpressionNode> right)
      : left_(std::move(left)), op_(op), right_(std::move(right)) {}

  void accept(Visitor &v) override { v.visit(*this); }
};