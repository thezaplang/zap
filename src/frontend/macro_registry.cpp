#include "frontend/macro_registry.hpp"

#include <filesystem>
#include <functional>
#include <set>
#include <utility>

namespace zap::frontend {

namespace {

bool sameLiteral(const TokenTree& left, const TokenTree& right) {
    if (left.isLeaf() != right.isLeaf())
        return false;
    if (left.isLeaf())
        return left.token().type == right.token().type
            && left.token().spelling == right.token().spelling;
    if (left.delimiter() != right.delimiter() || left.children().size() != right.children().size())
        return false;
    for (size_t index = 0; index < left.children().size(); ++index) {
        if (!sameLiteral(left.children()[index], right.children()[index]))
            return false;
    }
    return true;
}

bool sameSignature(const MacroDefinition& left, const MacroDefinition& right) {
    if (left.pattern.size() != right.pattern.size())
        return false;
    for (size_t index = 0; index < left.pattern.size(); ++index) {
        const auto& a = left.pattern[index];
        const auto& b = right.pattern[index];
        if (a.index() != b.index())
            return false;
        if (const auto* capture = std::get_if<MacroParameter>(&a)) {
            const auto& other = std::get<MacroParameter>(b);
            if (capture->kind != other.kind || capture->isVariadic != other.isVariadic)
                return false;
        } else if (!sameLiteral(std::get<TokenTree>(a), std::get<TokenTree>(b))) {
            return false;
        }
    }
    return true;
}

} // namespace

const MacroOverloadSet* MacroRegistry::find(const std::string& name) const {
    const auto it = visible_.find(name);
    return it == visible_.end() ? nullptr : &it->second;
}

const MacroOverloadSet* MacroRegistry::findExported(const std::string& name) const {
    const auto it = exported_.find(name);
    return it == exported_.end() ? nullptr : &it->second;
}

const std::string* MacroRegistry::findModule(const std::string& alias) const {
    const auto it = modules_.find(alias);
    return it == modules_.end() ? nullptr : &it->second;
}

MacroRegistrySet MacroRegistrySet::resolve(
    const std::map<std::string, ModuleOutline>& outlines,
    const ImportGraph& imports,
    std::vector<MacroResolutionError>& errors
) {
    MacroRegistrySet result;
    for (const auto& [moduleId, _] : outlines)
        result.modules_.emplace(moduleId, MacroRegistry{});

    std::map<std::string, int> states;
    std::function<void(const std::string&)> resolveModule = [&](const std::string& moduleId) {
        if (states[moduleId] == 2)
            return;
        if (states[moduleId] == 1)
            return; // The module graph reports import cycles separately.
        states[moduleId] = 1;

        auto& registry = result.modules_.at(moduleId);
        std::set<std::pair<std::string, size_t>> reportedConflicts;
        auto insertMacro = [&](std::map<std::string, MacroOverloadSet>& names,
                               const std::string& name,
                               const MacroBinding& binding,
                               const SourceSpan& span) {
            auto& overloads = names[name];
            for (const auto& existing : overloads) {
                if (existing.definition == binding.definition)
                    return;
                if (sameSignature(*existing.definition, *binding.definition)) {
                    if (reportedConflicts.emplace(name, span.offset).second) {
                        errors.push_back(
                            {moduleId,
                                span,
                                "Macro signature for '" + name
                                    + "' conflicts with another macro in this "
                                      "module."}
                        );
                    }
                    return;
                }
            }
            overloads.push_back(binding);
        };

        for (const auto& definition : outlines.at(moduleId).macros) {
            const MacroBinding binding{&definition, moduleId};
            insertMacro(registry.visible_, definition.name.value, binding, definition.name.span);
            if (definition.visibility == Visibility::Public) {
                insertMacro(
                    registry.exported_,
                    definition.name.value,
                    binding,
                    definition.name.span
                );
            }
        }

        auto insertFunction =
            [](auto& names, const std::string& name, const FunctionBinding& binding) {
                auto& functions = names[name];
                for (const auto& existing : functions)
                    if (existing.definition == binding.definition)
                        return;
                functions.push_back(binding);
            };
        for (const auto& definition : outlines.at(moduleId).functions) {
            const FunctionBinding binding{&definition, moduleId};
            insertFunction(registry.functions_, definition.name.value, binding);
            if (definition.visibility == Visibility::Public)
                insertFunction(registry.exportedFunctions_, definition.name.value, binding);
        }

        const auto importsIt = imports.find(moduleId);
        if (importsIt != imports.end()) {
            for (const auto& import : importsIt->second) {
                if ((!import.moduleAlias.empty() || !import.bindings.empty())
                    && import.targetModuleIds.size() != 1) {
                    continue;
                }
                for (const auto& targetId : import.targetModuleIds) {
                    auto targetIt = result.modules_.find(targetId);
                    if (targetIt == result.modules_.end())
                        continue;
                    resolveModule(targetId);
                    const auto& target = targetIt->second;
                    const std::string alias = import.moduleAlias.empty()
                        ? std::filesystem::path(targetId).stem().string()
                        : import.moduleAlias;
                    const auto [owner, inserted] = registry.modules_.emplace(alias, targetId);
                    if (!inserted && owner->second != targetId) {
                        errors.push_back(
                            {moduleId,
                                import.span,
                                "Macro module alias '" + alias + "' refers to multiple modules."}
                        );
                        continue;
                    }

                    if (import.bindings.empty()) {
                        const bool prelude =
                            import.rawPath == "std/prelude" && import.moduleAlias.empty();
                        for (const auto& [name, overloads] : target.exported_) {
                            const bool visibleFromPrelude =
                                prelude && registry.visible_.count(name) == 0;
                            for (const auto& binding : overloads) {
                                if (visibleFromPrelude) {
                                    insertMacro(registry.visible_, name, binding, import.span);
                                }
                                if (import.visibility == Visibility::Public) {
                                    insertMacro(registry.exported_, name, binding, import.span);
                                }
                            }
                        }
                        for (const auto& [name, functions] : target.exportedFunctions_) {
                            const bool visibleFromPrelude =
                                prelude && registry.functions_.count(name) == 0;
                            for (const auto& function : functions) {
                                if (visibleFromPrelude)
                                    insertFunction(registry.functions_, name, function);
                                if (import.visibility == Visibility::Public)
                                    insertFunction(registry.exportedFunctions_, name, function);
                            }
                        }
                        continue;
                    }

                    for (const auto& binding : import.bindings) {
                        const auto functions = target.exportedFunctions_.find(binding.sourceName);
                        if (functions != target.exportedFunctions_.end()) {
                            for (const auto& function : functions->second) {
                                insertFunction(registry.functions_, binding.localName, function);
                                if (import.visibility == Visibility::Public)
                                    insertFunction(
                                        registry.exportedFunctions_,
                                        binding.localName,
                                        function
                                    );
                            }
                        }
                        const MacroOverloadSet* macros = target.findExported(binding.sourceName);
                        if (!macros)
                            continue; // This binding may name a non-macro symbol.
                        for (const auto& macro : *macros) {
                            insertMacro(registry.visible_, binding.localName, macro, import.span);
                            if (import.visibility == Visibility::Public) {
                                insertMacro(
                                    registry.exported_,
                                    binding.localName,
                                    macro,
                                    import.span
                                );
                            }
                        }
                    }
                }
            }
        }
        states[moduleId] = 2;
    };

    for (const auto& [moduleId, _] : outlines)
        resolveModule(moduleId);
    result.prepareCtfe(outlines, errors);
    return result;
}

const MacroRegistry* MacroRegistrySet::module(const std::string& moduleId) const {
    const auto it = modules_.find(moduleId);
    return it == modules_.end() ? nullptr : &it->second;
}

const MacroOverloadSet* MacroRegistrySet::find(
    const std::string& moduleId,
    const std::string& name
) const {
    const MacroRegistry* registry = module(moduleId);
    return registry ? registry->find(name) : nullptr;
}

const MacroOverloadSet* MacroRegistrySet::findQualified(
    const std::string& moduleId,
    const std::string& alias,
    const std::string& name
) const {
    const MacroRegistry* registry = module(moduleId);
    if (!registry)
        return nullptr;
    const std::string* targetId = registry->findModule(alias);
    const MacroRegistry* target = targetId ? module(*targetId) : nullptr;
    return target ? target->findExported(name) : nullptr;
}

} // namespace zap::frontend
