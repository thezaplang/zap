#pragma once
#include "../visitor.hpp"

class ConstInt : public ExpressionNode {
public:
  int value_;
  std::string typeName_ = "i32";
  ConstInt() = default;
  ConstInt(int value, std::string typeName = "i32")
      : value_(value), typeName_(typeName) {}

  void accept(Visitor &v) override { v.visit(*this); }
};