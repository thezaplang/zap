#pragma once

#include "macros/macro_definition.hpp"

#include <string>
#include <vector>

namespace zap {

namespace ctfe {
class CtfeProgram;
}

struct MacroBinding {
  // Definitions must outlive the resolver and any expansion using it.
  const MacroDefinition *definition;
  std::string definingModuleId;
};

using MacroOverloadSet = std::vector<MacroBinding>;

class MacroResolver {
public:
  virtual ~MacroResolver() = default;
  virtual const MacroOverloadSet *find(const std::string &moduleId,
                                       const std::string &name) const = 0;
  virtual const MacroOverloadSet *
  findQualified(const std::string &moduleId, const std::string &alias,
                const std::string &name) const = 0;
  virtual const ctfe::CtfeProgram *program(const MacroDefinition &) const = 0;
};

} // namespace zap
