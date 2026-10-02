#pragma once

#include "macros/macro_meta.hpp"
#include "utils/diagnostics.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace zap {

class MacroTemplateExpander {
public:
  using SourceMacroLookup =
      std::function<bool(const std::vector<std::string> &, const Token &)>;
  MacroTemplateExpander(const MacroCaptures &captures,
                        const SourceSpan &invocation,
                        std::shared_ptr<const ExpansionOrigin> origin,
                        DiagnosticEngine &diagnostics, size_t maxTokens,
                        size_t maxIterations,
                        MacroMetaEvaluator::FreshContext freshContext,
                        SourceMacroLookup sourceMacroLookup);

  std::optional<std::vector<TokenTree>>
  expand(const std::vector<TokenTree> &templateTrees);

private:
  struct Loop;
  struct When;
  struct Case;
  struct Let;
  enum class Flow { Normal, Break, Continue };
  struct ExpansionResult {
    std::vector<TokenTree> trees;
    Flow flow = Flow::Normal;
  };

  const MacroCaptures &captures_;
  const SourceSpan &invocation_;
  std::shared_ptr<const ExpansionOrigin> origin_;
  DiagnosticEngine &diagnostics_;
  size_t maxTokens_;
  size_t maxIterations_;
  size_t emittedTokens_ = 0;
  size_t expandedIterations_ = 0;
  MacroMetaEvaluator meta_;
  SourceMacroLookup sourceMacroLookup_;

  std::optional<ExpansionResult>
  expandTrees(const std::vector<TokenTree> &trees, const MetaScope &scope,
              size_t loopDepth = 0);
  bool validateTrees(const std::vector<TokenTree> &trees,
                     const MetaScope &scope, size_t loopDepth = 0);
  std::optional<Loop> parseLoop(const std::vector<TokenTree> &trees,
                                size_t start);
  std::optional<When> parseWhen(const std::vector<TokenTree> &trees,
                                size_t start);
  std::optional<Case> parseCase(const std::vector<TokenTree> &trees,
                                size_t start);
  std::optional<Let> parseLet(const std::vector<TokenTree> &trees,
                              size_t start);
  bool reserve(size_t count);
  void report(const std::string &message, const char *code = nullptr);
};

} // namespace zap
