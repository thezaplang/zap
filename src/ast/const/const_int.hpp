#pragma once
#include "../expr_node.hpp"
#include "../visitor.hpp"

#include <cstdint>
#include <string>

class ConstInt : public ExpressionNode {
public:
  // Canonical textual representation of the integer literal as parsed.
  // This preserves full literal range (including values that may exceed int64).
  std::string value_;

  // Legacy compatibility fields:
  // - value_i64_ keeps the old signed payload when available
  // - has_value_i64_ tells whether value_i64_ is valid
  int64_t value_i64_ = 0;
  bool has_value_i64_ = false;

  // Kept for existing code paths that rely on this member.
  std::string typeName_ = "i32";

  ConstInt() = default;

  // Preferred constructor: preserve raw literal text.
  explicit ConstInt(std::string value, std::string typeName = "i32")
      : value_(std::move(value)), typeName_(std::move(typeName)) {}

  // Backward-compatible constructor for older parser/builder call sites.
  ConstInt(int64_t value, std::string typeName = "i32")
      : value_(std::to_string(value)), value_i64_(value), has_value_i64_(true),
        typeName_(std::move(typeName)) {}

  // Accessor compatible with old "signed payload" semantics.
  // If no signed payload is available, this returns 0.
  int64_t value() const { return has_value_i64_ ? value_i64_ : 0; }

  void accept(Visitor &v) override { v.visit(*this); }
};