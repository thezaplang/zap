#pragma once
#include "../expr_node.hpp"
#include "../visitor.hpp"

#include <string>
#include <type_traits>

class ConstString : public ExpressionNode {
public:
  std::string value_;

  ConstString() noexcept(
      std::is_nothrow_default_constructible<std::string>::value) = default;
  explicit ConstString(std::string value) : value_(std::move(value)) {}

  void accept(Visitor &v) override { v.visit(*this); }
};