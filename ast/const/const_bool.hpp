#pragma once
#include "../expr_node.hpp"
#include "../visitor.hpp"

class ConstBool : public ExpressionNode {
public:
  bool value_;
  ConstBool() = default;
  ConstBool(bool value) : value_(value) {}

  void accept(Visitor &v) override { v.visit(*this); }
};
