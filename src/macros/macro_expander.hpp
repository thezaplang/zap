#pragma once

#include "macros/macro_resolver.hpp"
#include "token/token_tree.hpp"
#include "utils/diagnostics.hpp"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace zap {

struct MacroLimits {
  size_t maxDepth = 128;
  size_t maxGeneratedTokens = 1'000'000;
  size_t maxTemplateIterations = 1'000'000;
  size_t maxMatchAttempts = 256;
};

struct MacroCall {
  std::vector<std::string> path;
  TokenTree arguments;
  SourceSpan span;
  std::shared_ptr<const ExpansionOrigin> parentOrigin;
};

class MacroExpander {
public:
  MacroExpander(const MacroResolver &registry, DiagnosticEngine &diagnostics,
                MacroLimits limits = {});

  std::optional<std::vector<TokenTree>> expand(const std::string &moduleId,
                                               const MacroCall &call);

private:
  const MacroResolver &registry_;
  DiagnosticEngine &diagnostics_;
  MacroLimits limits_;
  std::map<std::string, size_t> generatedTokensByModule_;

  std::optional<std::vector<TokenTree>>
  expandCall(const std::string &lookupModuleId,
             const std::string &outputModuleId, const MacroCall &call,
             size_t depth);
  std::optional<std::vector<TokenTree>> expandGenerated(
      const std::vector<TokenTree> &trees,
      const std::string &definitionModuleId, const std::string &outputModuleId,
      const std::shared_ptr<const ExpansionOrigin> &origin, size_t depth);
  void report(const SourceSpan &span, const std::string &message);
};

} // namespace zap
