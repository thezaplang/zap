#pragma once
#include "body_node.hpp"
#include "expr_node.hpp"
#include "statement_node.hpp"
#include "visitor.hpp"
#include <memory>
#include <string>

class ForInNode : public StatementNode {
public:
  std::string itemName_;
  std::unique_ptr<ExpressionNode> iterable_;
  std::unique_ptr<BodyNode> body_;

  ForInNode() noexcept = default;
  ForInNode(std::string itemName, std::unique_ptr<ExpressionNode> iterable,
            std::unique_ptr<BodyNode> body)
      : itemName_(std::move(itemName)), iterable_(std::move(iterable)),
        body_(std::move(body)) {}

  void accept(Visitor &v) override { v.visit(*this); }
};
