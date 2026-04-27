#pragma once
#include "assign_node.hpp"
#include "body_node.hpp"
#include "statement_node.hpp"
#include "var_decl.hpp"
#include "visitor.hpp"
#include <memory>

class ForNode : public StatementNode {
public:
  std::unique_ptr<VarDecl> initializer_;
  std::unique_ptr<ExpressionNode> condition_;
  std::unique_ptr<AssignNode> increment_;
  std::unique_ptr<BodyNode> body_;

  ForNode() noexcept = default;
  ForNode(std::unique_ptr<VarDecl> initializer,
          std::unique_ptr<ExpressionNode> condition,
          std::unique_ptr<AssignNode> increment, std::unique_ptr<BodyNode> body)
      : initializer_(std::move(initializer)), condition_(std::move(condition)),
        increment_(std::move(increment)), body_(std::move(body)) {}

  void accept(Visitor &v) override { v.visit(*this); }
};
