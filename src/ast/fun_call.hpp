#pragma once
#include "statement_node.hpp"
#include "type_node.hpp"
#include "visitor.hpp"

struct Argument {
  std::string name;
  SyntaxName syntaxName;
  std::unique_ptr<ExpressionNode> value;
  bool isRef = false;
  bool isSpread = false;

  Argument(const std::string &argName, std::unique_ptr<ExpressionNode> argValue,
           bool isRef = false, bool isSpread = false,
           SyntaxName syntaxName = {})
      : name(argName), syntaxName(std::move(syntaxName)),
        value(std::move(argValue)), isRef(isRef), isSpread(isSpread) {}
};

class FunCall : public ExpressionNode, public StatementNode {
public:
  std::unique_ptr<ExpressionNode> callee_;
  std::vector<std::unique_ptr<TypeNode>> genericArgs_;
  std::vector<std::unique_ptr<Argument>> params_;

  void accept(Visitor &v) override { v.visit(*this); }
};
