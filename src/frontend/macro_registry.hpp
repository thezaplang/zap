#pragma once

#include "frontend/module_outline.hpp"
#include "macros/macro_resolver.hpp"
#include "sema/module_info.hpp"

#include <map>
#include <string>
#include <vector>

namespace zap::frontend {

struct MacroResolutionError {
  std::string moduleId;
  SourceSpan span;
  std::string message;
};

class MacroRegistry {
public:
  const MacroOverloadSet *find(const std::string &name) const;
  const MacroOverloadSet *findExported(const std::string &name) const;
  const std::string *findModule(const std::string &alias) const;

private:
  friend class MacroRegistrySet;
  std::map<std::string, MacroOverloadSet> visible_;
  std::map<std::string, MacroOverloadSet> exported_;
  std::map<std::string, std::string> modules_;
};

class MacroRegistrySet : public MacroResolver {
public:
  using ImportGraph = std::map<std::string, std::vector<sema::ResolvedImport>>;

  static MacroRegistrySet
  resolve(const std::map<std::string, ModuleOutline> &outlines,
          const ImportGraph &imports,
          std::vector<MacroResolutionError> &errors);

  const MacroRegistry *module(const std::string &moduleId) const;
  const MacroOverloadSet *find(const std::string &moduleId,
                               const std::string &name) const override;
  const MacroOverloadSet *findQualified(const std::string &moduleId,
                                        const std::string &alias,
                                        const std::string &name) const override;

private:
  std::map<std::string, MacroRegistry> modules_;
};

} // namespace zap::frontend
