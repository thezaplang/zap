#pragma once

#include "token/token.hpp"

#include <string>
#include <vector>

namespace zap::frontend {

struct FrontendProject;

class ExpandedSyntaxEmitter {
public:
  static std::string render(const std::string &entryModuleId,
                            const std::string &moduleId,
                            const std::vector<Token> &tokens);

  // A bound project is lowered to ordinary, standalone Zap names. Unbound
  // projects retain the diagnostic token view, without claiming round-trip.
  static std::string renderProject(const FrontendProject &project);
};

} // namespace zap::frontend
