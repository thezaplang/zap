#pragma once
#include "../../token/syntax_name.hpp"
#include "../visitor.hpp"

class ConstId : public ExpressionNode {
public:
  std::string value_;
  SyntaxName syntaxName_;
  ConstId() noexcept(
      std::is_nothrow_default_constructible<std::string>::value) = default;
  ConstId(std::string value) : value_(std::move(value)), syntaxName_(value_) {}

  void accept(Visitor &v) override { v.visit(*this); }
};
