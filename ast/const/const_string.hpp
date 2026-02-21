#pragma once
#include "../visitor.hpp"

class ConstString : public ExpressionNode {
public:
  std::string value_;
  ConstString() = default;
  ConstString(std::string value) : value_(value) {}

  void accept(Visitor &v) override { v.visit(*this); }
};