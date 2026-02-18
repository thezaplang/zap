#pragma once
#include "node.hpp"
#include "expr_node.hpp"
#include <memory>
#include <string>

class TypeNode : public Node {
public:
  std::string typeName;
  bool isReference = false;
  bool isPointer = false;
  bool isArray = false;
  bool isVarArgs = false;
  std::unique_ptr<ExpressionNode> arraySize; // nullptr for non-array types

  TypeNode() = default;
  explicit TypeNode(const std::string &typeName_) : typeName(typeName_) {}
};