#pragma once
#include "statement_node.hpp"
#include "expr_node.hpp"
#include "body_node.hpp"
#include <memory>

class IfNode : public StatementNode {
public:
    std::unique_ptr<ExpressionNode> condition_;
    std::unique_ptr<BodyNode> thenBody_;
    std::unique_ptr<BodyNode> elseBody_;

    IfNode() = default;

   IfNode(std::unique_ptr<ExpressionNode> condition,
       std::unique_ptr<BodyNode> thenBody,
       std::unique_ptr<BodyNode> elseBody = nullptr)
    : condition_(std::move(condition)),
      thenBody_(std::move(thenBody)),
      elseBody_(std::move(elseBody)) {}
};
