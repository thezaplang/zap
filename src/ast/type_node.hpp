#pragma once
#include "expr_node.hpp"
#include "node.hpp"
#include "visitor.hpp"
#include <memory>
#include <string>
#include <vector>

class TypeNode : public Node {
public:
  std::string typeName;
  std::vector<std::string> qualifiers;
  bool isReference = false;
  bool isPointer = false;
  bool isArray = false;
  bool isVarArgs = false;
  std::unique_ptr<ExpressionNode> arraySize; // nullptr for non-array types
  std::unique_ptr<TypeNode> baseType; // For recursive types like arrays or pointers

  TypeNode() noexcept(std::is_nothrow_default_constructible<std::string>::value) = default;
  explicit TypeNode(const std::string &typeName_) : typeName(typeName_) {}

  std::string qualifiedName() const {
    if (qualifiers.empty()) {
      return typeName;
    }

    std::string result;
    for (size_t i = 0; i < qualifiers.size(); ++i) {
      if (i != 0) {
        result += ".";
      }
      result += qualifiers[i];
    }
    result += ".";
    result += typeName;
    return result;
  }

  void accept(Visitor &v) override { v.visit(*this); }
};
