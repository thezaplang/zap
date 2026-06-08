#pragma once

#include "expr_node.hpp"
#include <memory>
#include <string>
#include <vector>

enum class AttributeArgumentKind { Positional, Named };

struct AttributeArgument {
  AttributeArgumentKind kind = AttributeArgumentKind::Positional;
  std::string name;
  std::unique_ptr<ExpressionNode> value;
};

struct AttributeNode {
  std::string name;
  std::vector<AttributeArgument> arguments;
  SourceSpan span;

  bool hasArguments() const noexcept { return !arguments.empty(); }
  bool hasNamedArguments() const noexcept {
    for (const auto &arg : arguments) {
      if (arg.kind == AttributeArgumentKind::Named) {
        return true;
      }
    }
    return false;
  }
};