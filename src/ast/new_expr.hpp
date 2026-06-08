#pragma once
#include "expr_node.hpp"
#include "fun_call.hpp"
#include "type_node.hpp"
#include "visitor.hpp"
#include <memory>
#include <vector>

class NewExpr : public ExpressionNode {
public:
  std::unique_ptr<TypeNode> type_;
  std::vector<std::unique_ptr<Argument>> args_;

  explicit NewExpr(std::unique_ptr<TypeNode> type) : type_(std::move(type)) {}

  void accept(Visitor &v) override { v.visit(*this); }
};
