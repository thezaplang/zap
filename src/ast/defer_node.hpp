#pragma once
#include "node.hpp"
#include "statement_node.hpp"
#include "visitor.hpp"
#include <memory>

class DeferNode : public StatementNode {
public:
  std::unique_ptr<Node> statement_;

  DeferNode() noexcept = default;
  explicit DeferNode(std::unique_ptr<Node> statement)
      : statement_(std::move(statement)) {}

  void accept(Visitor &v) override { v.visit(*this); }
};