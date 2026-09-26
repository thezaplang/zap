#pragma once

#include "../token/token_tree.hpp"
#include "../visibility.hpp"

#include <string>
#include <vector>

namespace zap {

enum class MacroParameterKind {
  Identifier,
  Literal,
  Expression,
  Type,
  Statement,
  Block,
  Item,
  Tokens,
};

struct MacroParameter {
  Token name;
  MacroParameterKind kind;
  SourceSpan span;
};

struct MacroDefinition {
  Token name;
  Visibility visibility;
  std::vector<MacroParameter> parameters;
  TokenTree expansion;
  SourceSpan span;
};

} // namespace zap
