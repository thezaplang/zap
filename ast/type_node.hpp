#pragma once
#include "expr_node.hpp"
#include "node.hpp"
#include <iostream>
#include <memory>
#include <vector>

class TypeNode : public Node {
public:
  std::string typeName;
  bool isReference;
  bool isPointer;
  bool isArray;
  bool isVarArgs;
  ExpressionNode arraySize; // 0 for non-array types
  TypeNode(std::string typeName_)
      : typeName(typeName_), isReference(false), isPointer(false),
        isArray(false), isVarArgs(false) {}
};