#pragma once
#include "../token/syntax_name.hpp"
#include "../visibility.hpp"
#include "expr_node.hpp"
#include "node.hpp"
#include "type_node.hpp"
#include "visitor.hpp"
#include <iostream>
#include <memory>
#include <vector>

class ParameterNode : public Node {
public:
  std::string name;
  SyntaxName syntaxName;
  std::unique_ptr<TypeNode> type;
  std::unique_ptr<ExpressionNode> defaultValue;
  bool isRef = false;
  bool isSink = false;
  bool isNoEscape = false;
  bool isVariadic = false;
  Visibility visibility_ = Visibility::Private;

  ParameterNode(const std::string &name, std::unique_ptr<TypeNode> type,
                bool isRef = false, bool isSink = false,
                bool isNoEscape = false, bool isVariadic = false,
                std::unique_ptr<ExpressionNode> defaultValue = nullptr)
      : name(name), syntaxName(name), type(std::move(type)),
        defaultValue(std::move(defaultValue)), isRef(isRef), isSink(isSink),
        isNoEscape(isNoEscape), isVariadic(isVariadic) {}

  void accept(Visitor &v) override { v.visit(*this); }
};
