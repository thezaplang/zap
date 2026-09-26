#include "frontend/macro_registry.hpp"

#include <filesystem>
#include <functional>
#include <set>
#include <utility>

namespace zap::frontend {

const MacroBinding *MacroRegistry::find(const std::string &name) const {
  const auto it = visible_.find(name);
  return it == visible_.end() ? nullptr : &it->second;
}

const MacroBinding *MacroRegistry::findExported(const std::string &name) const {
  const auto it = exported_.find(name);
  return it == exported_.end() ? nullptr : &it->second;
}

const std::string *MacroRegistry::findModule(const std::string &alias) const {
  const auto it = modules_.find(alias);
  return it == modules_.end() ? nullptr : &it->second;
}

MacroRegistrySet
MacroRegistrySet::resolve(const std::map<std::string, ModuleOutline> &outlines,
                          const ImportGraph &imports,
                          std::vector<MacroResolutionError> &errors) {
  MacroRegistrySet result;
  for (const auto &[moduleId, _] : outlines)
    result.modules_.emplace(moduleId, MacroRegistry{});

  std::map<std::string, int> states;
  std::function<void(const std::string &)> resolveModule =
      [&](const std::string &moduleId) {
        if (states[moduleId] == 2)
          return;
        if (states[moduleId] == 1)
          return; // The module graph reports import cycles separately.
        states[moduleId] = 1;

        auto &registry = result.modules_.at(moduleId);
        std::set<std::pair<std::string, size_t>> reportedConflicts;
        auto insertMacro = [&](std::map<std::string, MacroBinding> &names,
                               const std::string &name,
                               const MacroBinding &binding,
                               const SourceSpan &span) {
          const auto [it, inserted] = names.emplace(name, binding);
          if (!inserted && it->second.definition != binding.definition &&
              reportedConflicts.emplace(name, span.offset).second) {
            errors.push_back({moduleId, span,
                              "Macro name '" + name +
                                  "' conflicts with another "
                                  "macro in this module."});
          }
        };

        for (const auto &definition : outlines.at(moduleId).macros) {
          const MacroBinding binding{&definition, moduleId};
          insertMacro(registry.visible_, definition.name.value, binding,
                      definition.name.span);
          if (definition.visibility == Visibility::Public) {
            insertMacro(registry.exported_, definition.name.value, binding,
                        definition.name.span);
          }
        }

        const auto importsIt = imports.find(moduleId);
        if (importsIt != imports.end()) {
          for (const auto &import : importsIt->second) {
            if ((!import.moduleAlias.empty() || !import.bindings.empty()) &&
                import.targetModuleIds.size() != 1) {
              continue;
            }
            for (const auto &targetId : import.targetModuleIds) {
              auto targetIt = result.modules_.find(targetId);
              if (targetIt == result.modules_.end())
                continue;
              resolveModule(targetId);
              const auto &target = targetIt->second;
              const std::string alias =
                  import.moduleAlias.empty()
                      ? std::filesystem::path(targetId).stem().string()
                      : import.moduleAlias;
              const auto [owner, inserted] =
                  registry.modules_.emplace(alias, targetId);
              if (!inserted && owner->second != targetId) {
                errors.push_back({moduleId, import.span,
                                  "Macro module alias '" + alias +
                                      "' refers to multiple modules."});
                continue;
              }

              if (import.bindings.empty()) {
                const bool prelude = import.rawPath == "std/prelude" &&
                                     import.moduleAlias.empty();
                for (const auto &[name, binding] : target.exported_) {
                  if (prelude && registry.visible_.count(name) == 0) {
                    registry.visible_.emplace(name, binding);
                  }
                  if (import.visibility == Visibility::Public) {
                    insertMacro(registry.exported_, name, binding, import.span);
                  }
                }
                continue;
              }

              for (const auto &binding : import.bindings) {
                const MacroBinding *macro =
                    target.findExported(binding.sourceName);
                if (!macro)
                  continue; // This binding may name a non-macro symbol.
                insertMacro(registry.visible_, binding.localName, *macro,
                            import.span);
                if (import.visibility == Visibility::Public) {
                  insertMacro(registry.exported_, binding.localName, *macro,
                              import.span);
                }
              }
            }
          }
        }
        states[moduleId] = 2;
      };

  for (const auto &[moduleId, _] : outlines)
    resolveModule(moduleId);
  return result;
}

const MacroRegistry *
MacroRegistrySet::module(const std::string &moduleId) const {
  const auto it = modules_.find(moduleId);
  return it == modules_.end() ? nullptr : &it->second;
}

const MacroBinding *MacroRegistrySet::find(const std::string &moduleId,
                                           const std::string &name) const {
  const MacroRegistry *registry = module(moduleId);
  return registry ? registry->find(name) : nullptr;
}

const MacroBinding *
MacroRegistrySet::findQualified(const std::string &moduleId,
                                const std::string &alias,
                                const std::string &name) const {
  const MacroRegistry *registry = module(moduleId);
  if (!registry)
    return nullptr;
  const std::string *targetId = registry->findModule(alias);
  const MacroRegistry *target = targetId ? module(*targetId) : nullptr;
  return target ? target->findExported(name) : nullptr;
}

} // namespace zap::frontend
