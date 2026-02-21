#pragma once
#include "../visitor.hpp"

class ConstFloat : public ExpressionNode {
public:
  float value_;
  ConstFloat() = default;
  ConstFloat(float value) : value_(value) {}

  void accept(Visitor &v) override { v.visit(*this); }
};