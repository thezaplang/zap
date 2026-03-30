#pragma once
#include "../visitor.hpp"

class ConstFloat : public ExpressionNode {
public:
  double value_;
  ConstFloat() noexcept = default;
  ConstFloat(double value) noexcept : value_(value) {}

  void accept(Visitor &v) override { v.visit(*this); }
};