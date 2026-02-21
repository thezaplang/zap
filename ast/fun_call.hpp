#pragma once
#include "visitor.hpp"

struct Argument {
  std::string name;
  std::unique_ptr<ExpressionNode> value;

  Argument(const std::string &argName, std::unique_ptr<ExpressionNode> argValue)
      : name(argName), value(std::move(argValue)) {}
};

class FunCall : public ExpressionNode, public StatementNode {
public:
  std::string funcName_;
  std::vector<std::unique_ptr<Argument>> params_;

  void accept(Visitor &v) override { v.visit(*this); }
};
