#pragma once
#include "../token/syntax_name.hpp"
#include "body_node.hpp"
#include "generic_constraint.hpp"
#include "node.hpp"
#include "parameter_node.hpp"
#include "top_level.hpp"
#include "type_node.hpp"
#include "visitor.hpp"
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

enum class ExtensionReceiverMode {
  None,
  Value,
  Ref,
};

class FunDecl : public TopLevel {
public:
  std::string name_;
  SyntaxName syntaxName_;
  std::vector<std::unique_ptr<TypeNode>> genericParams_;
  std::vector<GenericConstraint> genericConstraints_;
  std::vector<std::unique_ptr<ParameterNode>> params_;
  std::unique_ptr<TypeNode> returnType_;
  std::unique_ptr<BodyNode> body_;
  std::unique_ptr<ExpressionNode> lambdaExpr_;
  std::optional<std::string> resultBorrowSource_;
  bool isExtern_ = false;
  bool isStatic_ = false;
  bool isUnsafe_ = false;
  bool returnsRef_ = false;
  ExtensionReceiverMode extensionReceiverMode_ = ExtensionReceiverMode::None;
  SourceSpan extensionReceiverSpan_;

  FunDecl() noexcept(
      std::is_nothrow_default_constructible<std::string>::value) = default;

  FunDecl(const std::string &name,
          std::vector<std::unique_ptr<TypeNode>> genericParams,
          std::vector<std::unique_ptr<ParameterNode>> params,
          std::unique_ptr<TypeNode> returnType, std::unique_ptr<BodyNode> body,
          std::unique_ptr<ExpressionNode> lambdaExpr, bool isExtern = false,
          bool isStatic = false, bool isUnsafe = false)
      : name_(name), syntaxName_(name),
        genericParams_(std::move(genericParams)), params_(std::move(params)),
        returnType_(std::move(returnType)), body_(std::move(body)),
        lambdaExpr_(std::move(lambdaExpr)), isExtern_(isExtern),
        isStatic_(isStatic), isUnsafe_(isUnsafe) {}

  void accept(Visitor &v) override { v.visit(*this); }
};
