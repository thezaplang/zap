#pragma once
#include "../visitor.hpp"

class ConstId : public ExpressionNode {
public:
  std::string value_;
  ConstId() = default;
  ConstId(std::string value) : value_(value) {}

  void accept(Visitor &v) override { v.visit(*this); }
};