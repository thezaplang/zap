#pragma once

#include "macros/macro_definition.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace zap {

struct MacroCapture {
  std::vector<std::vector<TokenTree>> elements;
  // A final separator represents a trailing comma in the invocation.
  std::vector<TokenTree> separators;
  MacroParameterKind kind = MacroParameterKind::Tokens;
  bool isVariadic = false;
  std::shared_ptr<const SourceFragment> source;
};

using MacroCaptures = std::map<std::string, MacroCapture>;

struct MetaFragment {
  const std::vector<TokenTree> *tokens = nullptr;
  MacroParameterKind kind = MacroParameterKind::Tokens;
  std::shared_ptr<const std::vector<TokenTree>> owner;
  std::shared_ptr<const SourceFragment> source;
};

struct MetaPosition {
  size_t index = 0;
  size_t count = 0;
};

struct MetaSyntaxKind {
  std::string name;
};

using MetaValue = std::variant<MetaFragment, const MacroCapture *, MetaPosition,
                               bool, int64_t, std::string, SourceSpan,
                               MetaSyntaxKind, std::monostate>;

struct MetaScope {
  const MetaScope *parent = nullptr;
  std::map<std::string, MetaValue> bindings;

  const MetaValue *find(const std::string &name) const {
    const auto it = bindings.find(name);
    if (it != bindings.end())
      return &it->second;
    return parent ? parent->find(name) : nullptr;
  }
};

} // namespace zap
