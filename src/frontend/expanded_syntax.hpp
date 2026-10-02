#pragma once

#include "macros/macro_expander.hpp"
#include "token/token.hpp"

#include <optional>
#include <string>
#include <vector>

namespace zap::frontend {

class ExpandedSyntaxEmitter {
public:
  ExpandedSyntaxEmitter(const MacroResolver &macros,
                        DiagnosticEngine &diagnostics);

  std::optional<std::vector<Token>> expand(const std::string &moduleId,
                                           const std::vector<Token> &tokens);

  static std::string render(const std::string &entryModuleId,
                            const std::string &moduleId,
                            const std::vector<Token> &tokens);

private:
  const MacroResolver &macros_;
  DiagnosticEngine &diagnostics_;
};

} // namespace zap::frontend
