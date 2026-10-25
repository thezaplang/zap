#include "ast/fun_decl.hpp"
#include "frontend/expanded_syntax.hpp"
#include "frontend/frontend_session.hpp"
#include "ir/type_identity.hpp"

#include <map>
#include <unordered_map>
#include <unordered_set>

namespace zap::frontend {
namespace {

bool runtimeModule(const sema::ModuleInfo& module) {
    return module.linkPath == "core" || module.linkPath.rfind("std/", 0) == 0;
}

struct PrintedName {
    std::string identifier;
    std::string member;
};

class ExpandedNames {
public:
    explicit ExpandedNames(const FrontendProject& project)
        : semantics_(project.semanticInfo) {
        for (const auto& [id, module] : project.modules)
            for (const auto& token : module->expandedTokens)
                if (token.type == TokenType::ID) {
                    reserved_.insert(token.value);
                    if (runtimeModule(*module))
                        runtimeOccurrences_.insert(token.occurrence.get());
                }

        // Overloads share a source name; module IDs, not file stems, identify
        // declaration families. Members deliberately never enter this table.
        std::map<std::pair<std::string, std::string>, std::string> families;
        for (const auto& [id, module] : project.modules) {
            std::string runtimeAlias;
            if (runtimeModule(*module)) {
                runtimeAlias = allocate();
                imports_ += "import \"" + module->linkPath + "\" as " + runtimeAlias + ";\n";
            }
            for (const auto& child : module->root->children) {
                auto declaration = dynamic_cast<const TopLevel*>(child.get());
                if (const auto* function = dynamic_cast<const FunDecl*>(child.get())) {
                    if (function->isCtfeOnly()) {
                        omit(*function);
                        continue;
                    }
                }
                if (!runtimeModule(*module) && dynamic_cast<const ImportNode*>(child.get())) {
                    omit(*declaration);
                    continue;
                }
                auto symbol = semantics_.symbolFor(child.get());
                if (!declaration || !declaration->declarationName_.occurrence || !symbol)
                    continue;
                if (!runtimeAlias.empty()) {
                    globals_[symbol.get()] = {runtimeAlias, symbol->name};
                    continue;
                }
                const auto key = std::make_pair(id, symbol->name);
                auto found = families.find(key);
                if (found == families.end()) {
                    bool keep = symbol->hasNoMangle || symbol->hasExternC;
                    if (auto function = std::dynamic_pointer_cast<sema::FunctionSymbol>(symbol))
                        keep = keep || function->isExternal || function->hasEntry
                            || (id == project.entryModuleId && function->name == "main");
                    if (auto variable = std::dynamic_pointer_cast<sema::VariableSymbol>(symbol))
                        keep = keep || variable->is_external;
                    found = families.emplace(key, keep ? symbol->name : allocate()).first;
                }
                globals_[symbol.get()] = {found->second, {}};
                declarations_[declaration->declarationName_.occurrence.get()] = found->second;
                if (auto variable = std::dynamic_pointer_cast<sema::VariableSymbol>(symbol)) {
                    if (variable->is_external) {
                        auto existing = externVariables_.find(variable->linkName);
                        if (existing != externVariables_.end()
                            && zir::sameType(existing->second->type, variable->type))
                            omit(*declaration);
                        else
                            externVariables_.emplace(variable->linkName, variable);
                    }
                }
                if (auto function = std::dynamic_pointer_cast<sema::FunctionSymbol>(symbol)) {
                    if (!function->isExternal)
                        continue;
                    std::shared_ptr<sema::FunctionSymbol> canonical;
                    for (const auto& prototype : externs_) {
                        if (prototype->linkName == function->linkName
                            && sema::sameFunctionSignature(*prototype, *function)
                            && zir::sameType(prototype->returnType, function->returnType)
                            && prototype->resultBorrow == function->resultBorrow
                            && prototype->returnsRef == function->returnsRef
                            && sameParameterContracts(*prototype, *function)) {
                            canonical = prototype;
                            break;
                        }
                    }
                    if (!canonical) {
                        externs_.push_back(function);
                        continue;
                    }
                    omit(*declaration);
                    for (size_t i = 0; i < function->parameters.size(); ++i) {
                        const auto& from = function->parameters[i]->syntaxName.occurrence;
                        const auto& to = canonical->parameters[i]->syntaxName.occurrence;
                        if (from && to)
                            canonicalOccurrences_[from.get()] = to.get();
                    }
                }
            }
        }

        // Allocate by declaration occurrence, so generic instantiations and copied
        // captures do not turn one source binding into several printed names.
        for (const auto& [id, module] : project.modules) {
            if (runtimeModule(*module))
                continue;
            for (const auto& token : module->expandedTokens) {
                auto binding = semantics_.resolvedNames.find(token.occurrence.get());
                if (binding == semantics_.resolvedNames.end())
                    continue;
                auto variable =
                    std::dynamic_pointer_cast<sema::VariableSymbol>(binding->second.symbol);
                if (variable && !globals_.count(variable.get()) && variable->syntaxName.occurrence
                    && !runtimeOccurrences_.count(variable->syntaxName.occurrence.get())) {
                    const auto identity =
                        canonicalOccurrence(variable->syntaxName.occurrence.get());
                    if (!declarations_.count(identity))
                        declarations_.emplace(identity, allocate());
                }
            }
        }
    }

    const std::string& imports() const { return imports_; }

    std::vector<Token> lower(const std::vector<Token>& tokens) const {
        std::vector<Token> result;
        for (size_t i = 0; i < tokens.size(); ++i) {
            const auto& token = tokens[i];
            auto omitted = omitted_.find(token.occurrence.get());
            if (omitted != omitted_.end()) {
                while (i < tokens.size() && tokens[i].occurrence.get() != omitted->second)
                    ++i;
                continue;
            }
            Token lowered = token;
            auto binding = semantics_.resolvedNames.find(token.occurrence.get());
            PrintedName name;
            size_t components = 1;
            if (binding != semantics_.resolvedNames.end()) {
                name = nameFor(binding->second.symbol);
                components = binding->second.components;
            }
            auto declaration = declarations_.find(token.occurrence.get());
            if (declaration != declarations_.end())
                name = {declaration->second, {}};
            if (!name.identifier.empty()) {
                if (semantics_.handlerBindings.count(token.occurrence.get())
                    && (i == 0 || tokens[i - 1].type != TokenType::VAR))
                    result.emplace_back(
                        TokenType::VAR,
                        "var",
                        token.span,
                        "var",
                        token.syntaxContext,
                        token.expansionOrigin
                    );
                if (semantics_.recordShorthands.count(token.occurrence.get())) {
                    result.push_back(token);
                    result.emplace_back(
                        TokenType::COLON,
                        ":",
                        token.span,
                        ":",
                        token.syntaxContext,
                        token.expansionOrigin
                    );
                }
                lowered.value = lowered.spelling = name.identifier;
                i += 2 * (components - 1);
            }
            result.push_back(std::move(lowered));
            if (!name.member.empty()) {
                result.emplace_back(
                    TokenType::DOT,
                    ".",
                    token.span,
                    ".",
                    token.syntaxContext,
                    token.expansionOrigin
                );
                result.emplace_back(
                    TokenType::ID,
                    name.member,
                    token.span,
                    name.member,
                    token.syntaxContext,
                    token.expansionOrigin
                );
            }
        }
        return result;
    }

private:
    void omit(const TopLevel& declaration) {
        const auto& range = declaration.syntaxRange_;
        // Implicit prelude imports have no source tokens to remove.
        if (range.first && range.last)
            omitted_[range.first.get()] = range.last.get();
    }

    static bool sameParameterContracts(
        const sema::FunctionSymbol& left,
        const sema::FunctionSymbol& right
    ) {
        for (size_t i = 0; i < left.parameters.size(); ++i)
            if (left.parameters[i]->is_sink != right.parameters[i]->is_sink
                || left.parameters[i]->is_noescape != right.parameters[i]->is_noescape)
                return false;
        return true;
    }

    const SyntaxOccurrence* canonicalOccurrence(const SyntaxOccurrence* identity) const {
        auto found = canonicalOccurrences_.find(identity);
        return found == canonicalOccurrences_.end() ? identity : found->second;
    }

    std::string allocate() {
        std::string name;
        do {
            name = "__zap_expanded_" + std::to_string(next_++);
        } while (!reserved_.insert(name).second);
        return name;
    }

    PrintedName nameFor(const std::shared_ptr<sema::Symbol>& symbol) const {
        auto global = globals_.find(symbol.get());
        if (global != globals_.end())
            return global->second;
        if (auto overloads = std::dynamic_pointer_cast<sema::OverloadSetSymbol>(symbol)) {
            for (const auto& function : overloads->overloads) {
                auto found = globals_.find(function.get());
                if (found != globals_.end())
                    return found->second;
            }
        }
        if (auto variable = std::dynamic_pointer_cast<sema::VariableSymbol>(symbol)) {
            auto found =
                declarations_.find(canonicalOccurrence(variable->syntaxName.occurrence.get()));
            if (found != declarations_.end())
                return {found->second, {}};
        }
        return {};
    }

    const sema::SemanticInfo& semantics_;
    std::unordered_set<std::string> reserved_;
    std::unordered_set<const SyntaxOccurrence*> runtimeOccurrences_;
    std::unordered_map<const sema::Symbol*, PrintedName> globals_;
    std::unordered_map<const SyntaxOccurrence*, std::string> declarations_;
    std::unordered_map<const SyntaxOccurrence*, const SyntaxOccurrence*> canonicalOccurrences_;
    std::unordered_map<const SyntaxOccurrence*, const SyntaxOccurrence*> omitted_;
    std::vector<std::shared_ptr<sema::FunctionSymbol>> externs_;
    std::unordered_map<std::string, std::shared_ptr<sema::VariableSymbol>> externVariables_;
    size_t next_ = 0;
    std::string imports_;
};

} // namespace

std::string ExpandedSyntaxEmitter::renderProject(const FrontendProject& project) {
    std::string result;
    if (!project.boundRoot) {
        result = "// unbound expansion: diagnostic view, not a hygienic round-trip\n";
        for (const auto& [id, module] : project.modules)
            result += render(project.entryModuleId, id, module->expandedTokens);
        return result;
    }
    const ExpandedNames names(project);
    result = names.imports();
    for (const auto& [id, module] : project.modules)
        if (!runtimeModule(*module))
            result += render(project.entryModuleId, id, names.lower(module->expandedTokens));
    return result;
}

} // namespace zap::frontend
