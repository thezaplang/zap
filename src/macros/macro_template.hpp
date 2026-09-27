#pragma once

#include "token/token_tree.hpp"
#include "utils/diagnostics.hpp"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace zap {

struct MacroCapture {
  std::vector<std::vector<TokenTree>> elements;
  // A final separator represents a trailing comma in the invocation.
  std::vector<TokenTree> separators;
  bool isVariadic = false;
};

using MacroCaptures = std::map<std::string, MacroCapture>;

class MacroTemplateExpander {
public:
  MacroTemplateExpander(const MacroCaptures &captures,
                        const SourceSpan &invocation,
                        std::shared_ptr<const ExpansionOrigin> origin,
                        DiagnosticEngine &diagnostics, size_t maxTokens,
                        size_t maxIterations);

  std::optional<std::vector<TokenTree>>
  expand(const std::vector<TokenTree> &templateTrees);

private:
  struct Scope;
  struct Loop;

  const MacroCaptures &captures_;
  const SourceSpan &invocation_;
  std::shared_ptr<const ExpansionOrigin> origin_;
  DiagnosticEngine &diagnostics_;
  size_t maxTokens_;
  size_t maxIterations_;
  size_t emittedTokens_ = 0;
  size_t expandedIterations_ = 0;

  std::optional<std::vector<TokenTree>>
  expandTrees(const std::vector<TokenTree> &trees, const Scope &scope);
  bool validateTrees(const std::vector<TokenTree> &trees, const Scope &scope);
  std::optional<Loop> parseLoop(const std::vector<TokenTree> &trees,
                                size_t start);
  bool reserve(size_t count);
  void report(const std::string &message);
};

} // namespace zap
