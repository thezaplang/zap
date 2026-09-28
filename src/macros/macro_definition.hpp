#pragma once

#include "../token/token_tree.hpp"
#include "../visibility.hpp"

#include <string>
#include <variant>
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
  bool isVariadic = false;
};

using MacroPatternPart = std::variant<TokenTree, MacroParameter>;

struct MacroDefinition {
  Token name;
  Visibility visibility;
  std::vector<MacroParameter> parameters;
  std::vector<MacroPatternPart> pattern;
  bool customPattern = false;
  TokenTree expansion;
  SourceSpan span;
};

} // namespace zap
