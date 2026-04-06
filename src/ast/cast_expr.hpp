#pragma once
#include "expr_node.hpp"
#include "type_node.hpp"
#include "visitor.hpp"
#include <memory>

class CastExpr : public ExpressionNode {
public:
  std::unique_ptr<ExpressionNode> expr_;
  std::unique_ptr<TypeNode> type_;

  CastExpr(std::unique_ptr<ExpressionNode> expr, std::unique_ptr<TypeNode> type)
      : expr_(std::move(expr)), type_(std::move(type)) {}

  void accept(Visitor &v) override { v.visit(*this); }
};
