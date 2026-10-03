#pragma once

#include "frontend/module_outline.hpp"
#include "macros/macro_resolver.hpp"
#include "sema/module_info.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace zap::frontend {

struct MacroResolutionError {
  std::string moduleId;
  SourceSpan span;
  std::string message;
  std::string code = "M1001";
};

struct FunctionBinding {
  const FunctionOutline *definition;
  std::string definingModuleId;
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
  std::map<std::string, std::vector<FunctionBinding>> functions_;
  std::map<std::string, std::vector<FunctionBinding>> exportedFunctions_;
};

class MacroRegistrySet : public MacroResolver {
public:
  using ImportGraph = std::map<std::string, std::vector<sema::ResolvedImport>>;

  static MacroRegistrySet
  resolve(const std::map<std::string, ModuleOutline> &outlines,
          const ImportGraph &imports,
          std::vector<MacroResolutionError> &errors);

  const MacroRegistry *module(const std::string &moduleId) const;
  const ctfe::CtfeProgram *
  program(const MacroDefinition &definition) const override;
  const MacroOverloadSet *find(const std::string &moduleId,
                               const std::string &name) const override;
  const MacroOverloadSet *findQualified(const std::string &moduleId,
                                        const std::string &alias,
                                        const std::string &name) const override;

private:
  void prepareCtfe(const std::map<std::string, ModuleOutline> &outlines,
                   std::vector<MacroResolutionError> &errors);
  std::map<std::string, MacroRegistry> modules_;
  std::map<const MacroDefinition *, std::shared_ptr<const ctfe::CtfeProgram>>
      programs_;
};

} // namespace zap::frontend
