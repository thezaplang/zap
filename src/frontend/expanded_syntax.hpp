#pragma once

#include "macros/macro_expander.hpp"
#include "token/token.hpp"

#include <optional>
#include <string>
#include <vector>

namespace zap::frontend {

struct FrontendProject;

class ExpandedSyntaxEmitter {
public:
  ExpandedSyntaxEmitter(const MacroResolver &macros,
                        DiagnosticEngine &diagnostics);

  std::optional<std::vector<Token>> expand(const std::string &moduleId,
                                           const std::vector<Token> &tokens);

  static std::string render(const std::string &entryModuleId,
                            const std::string &moduleId,
                            const std::vector<Token> &tokens);

  // A bound project is lowered to ordinary, standalone Zap names. Unbound
  // projects retain the diagnostic token view, without claiming round-trip.
  static std::string renderProject(const FrontendProject &project);

private:
  const MacroResolver &macros_;
  DiagnosticEngine &diagnostics_;
};

} // namespace zap::frontend
