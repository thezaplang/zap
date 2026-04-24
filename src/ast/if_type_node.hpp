#pragma once
#include "body_node.hpp"
#include "statement_node.hpp"
#include "type_node.hpp"
#include "visitor.hpp"
#include <memory>
#include <string>

class IfTypeNode : public StatementNode {
public:
  std::string parameterName_;
  std::unique_ptr<TypeNode> matchType_;
  std::unique_ptr<BodyNode> thenBody_;
  std::unique_ptr<BodyNode> elseBody_;

  IfTypeNode() noexcept = default;

  IfTypeNode(std::string parameterName, std::unique_ptr<TypeNode> matchType,
             std::unique_ptr<BodyNode> thenBody,
             std::unique_ptr<BodyNode> elseBody = nullptr)
      : parameterName_(std::move(parameterName)),
        matchType_(std::move(matchType)), thenBody_(std::move(thenBody)),
        elseBody_(std::move(elseBody)) {}

  void accept(Visitor &v) override { v.visit(*this); }
};
