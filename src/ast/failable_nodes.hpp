#pragma once

#include "body_node.hpp"
#include "expr_node.hpp"
#include "statement_node.hpp"
#include "visitor.hpp"
#include <memory>
#include <string>

class TryExpr : public ExpressionNode {
public:
  std::unique_ptr<ExpressionNode> expression_;

  TryExpr() noexcept = default;
  explicit TryExpr(std::unique_ptr<ExpressionNode> expression)
      : expression_(std::move(expression)) {}

  void accept(Visitor &v) override { v.visit(*this); }
};

class FallbackExpr : public ExpressionNode {
public:
  std::unique_ptr<ExpressionNode> expression_;
  std::unique_ptr<ExpressionNode> fallback_;

  FallbackExpr() noexcept = default;
  FallbackExpr(std::unique_ptr<ExpressionNode> expression,
               std::unique_ptr<ExpressionNode> fallback)
      : expression_(std::move(expression)), fallback_(std::move(fallback)) {}

  void accept(Visitor &v) override { v.visit(*this); }
};

class FailableHandleExpr : public ExpressionNode {
public:
  std::unique_ptr<ExpressionNode> expression_;
  std::string errorName_;
  std::unique_ptr<BodyNode> handler_;

  FailableHandleExpr() noexcept = default;
  FailableHandleExpr(std::unique_ptr<ExpressionNode> expression,
                     std::string errorName, std::unique_ptr<BodyNode> handler)
      : expression_(std::move(expression)), errorName_(std::move(errorName)),
        handler_(std::move(handler)) {}

  void accept(Visitor &v) override { v.visit(*this); }
};

class FailNode : public StatementNode {
public:
  std::unique_ptr<ExpressionNode> errorValue_;

  FailNode() noexcept = default;
  explicit FailNode(std::unique_ptr<ExpressionNode> errorValue)
      : errorValue_(std::move(errorValue)) {}

  ~FailNode() noexcept override = default;

  void accept(Visitor &v) override { v.visit(*this); }
};