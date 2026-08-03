#pragma once

#include "../binding_kind.hpp"
#include "expr_node.hpp"
#include "statement_node.hpp"
#include "top_level.hpp"
#include "type_node.hpp"
#include "visitor.hpp"
#include <string>

class BindingDecl : public StatementNode, public TopLevel {
public:
  std::string name_;
  std::unique_ptr<TypeNode> type_;
  std::unique_ptr<ExpressionNode> initializer_;
  BindingKind kind_ = BindingKind::Mutable;
  bool isGlobal_ = false;
  bool isExternal_ = false;

  BindingDecl() noexcept(
      std::is_nothrow_default_constructible<std::string>::value) = default;
  BindingDecl(std::string name, std::unique_ptr<TypeNode> type,
              std::unique_ptr<ExpressionNode> initializer, BindingKind kind)
      : name_(std::move(name)), type_(std::move(type)),
        initializer_(std::move(initializer)), kind_(kind) {}

  void accept(Visitor &v) override { v.visit(*this); }
};
