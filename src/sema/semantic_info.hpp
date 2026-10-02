#pragma once

#include "../ast/node.hpp"
#include "../ast/top_level.hpp"
#include "../ir/type.hpp"
#include "../token/token.hpp"
#include "symbol.hpp"
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace sema {

struct SemanticInfo {
  struct ResolvedName {
    std::shared_ptr<Symbol> symbol;
    size_t components = 1;
  };
  std::unordered_map<const SyntaxOccurrence *, ResolvedName> resolvedNames;
  std::unordered_map<const SyntaxOccurrence *, std::string> recordShorthands;
  std::unordered_set<const SyntaxOccurrence *> handlerBindings;

  void recordHandler(const SyntaxName &name) {
    if (name.occurrence)
      handlerBindings.insert(name.occurrence.get());
  }

  void recordShorthand(const SyntaxName &name, const std::string &field) {
    if (name.occurrence)
      recordShorthands[name.occurrence.get()] = field;
  }

  void recordName(const SyntaxName &name, const std::shared_ptr<Symbol> &symbol,
                  size_t components = 1) {
    if (name.occurrence && symbol)
      resolvedNames[name.occurrence.get()] = {symbol, components};
  }
  struct ResolvedCall {
    SourceSpan span;
    std::shared_ptr<FunctionSymbol> symbol;
  };
  struct ImportedSymbol {
    std::string targetModuleId;
    std::shared_ptr<Symbol> symbol;
  };

  std::unordered_map<const Node *, std::shared_ptr<Symbol>> symbolsByNode;
  std::unordered_map<const Symbol *, const Node *> declarationsBySymbol;
  std::unordered_map<const Node *, std::shared_ptr<zir::Type>> typesByNode;
  std::unordered_map<std::string, ImportedSymbol> importedSymbols;
  std::unordered_map<std::string, std::string> importedModules;
  std::unordered_map<std::string, std::vector<ResolvedCall>> resolvedCalls;

  void recordCall(const std::string &moduleId, SourceSpan span,
                  std::shared_ptr<FunctionSymbol> symbol) {
    if (symbol) {
      resolvedCalls[moduleId].push_back({span, std::move(symbol)});
    }
  }

  std::shared_ptr<FunctionSymbol> callAt(const std::string &moduleId,
                                         size_t offset) const {
    auto calls = resolvedCalls.find(moduleId);
    if (calls == resolvedCalls.end()) {
      return nullptr;
    }
    for (const auto &call : calls->second) {
      if (offset >= call.span.offset &&
          offset <= call.span.offset + call.span.length) {
        return call.symbol;
      }
    }
    return nullptr;
  }

  static std::string importKey(const std::string &moduleId,
                               const std::string &localName) {
    return moduleId + '\n' + localName;
  }

  void recordImportedSymbol(const std::string &moduleId,
                            const std::string &localName,
                            std::string targetModuleId,
                            std::shared_ptr<Symbol> symbol) {
    if (symbol) {
      importedSymbols[importKey(moduleId, localName)] = {
          std::move(targetModuleId), std::move(symbol)};
    }
  }

  const ImportedSymbol *importedSymbolFor(const std::string &moduleId,
                                          const std::string &localName) const {
    auto it = importedSymbols.find(importKey(moduleId, localName));
    return it == importedSymbols.end() ? nullptr : &it->second;
  }

  void recordImportedModule(const std::string &moduleId,
                            const std::string &localName,
                            std::string targetModuleId) {
    importedModules[importKey(moduleId, localName)] = std::move(targetModuleId);
  }

  const std::string *importedModuleFor(const std::string &moduleId,
                                       const std::string &localName) const {
    auto it = importedModules.find(importKey(moduleId, localName));
    return it == importedModules.end() ? nullptr : &it->second;
  }

  void recordSymbol(const Node *node, std::shared_ptr<Symbol> symbol) {
    if (node && symbol) {
      symbolsByNode[node] = std::move(symbol);
    }
  }

  void recordDeclaration(const Node *node,
                         const std::shared_ptr<Symbol> &symbol) {
    if (node && symbol) {
      symbolsByNode[node] = symbol;
      declarationsBySymbol[symbol.get()] = node;
      if (auto topLevel = dynamic_cast<const TopLevel *>(node))
        recordName(topLevel->declarationName_, symbol);
      if (auto variable = std::dynamic_pointer_cast<VariableSymbol>(symbol))
        recordName(variable->syntaxName, symbol);
    }
  }

  std::shared_ptr<Symbol>
  declarationNamed(const std::string &name,
                   const std::string &moduleName) const {
    for (const auto &[node, symbol] : symbolsByNode) {
      if (symbol && symbol->name == name && symbol->moduleName == moduleName &&
          declarationFor(symbol) == node) {
        return symbol;
      }
    }
    return nullptr;
  }

  const Node *declarationFor(const std::shared_ptr<Symbol> &symbol) const {
    if (!symbol) {
      return nullptr;
    }
    auto it = declarationsBySymbol.find(symbol.get());
    return it == declarationsBySymbol.end() ? nullptr : it->second;
  }

  void recordType(const Node *node, std::shared_ptr<zir::Type> type) {
    if (node && type) {
      typesByNode[node] = std::move(type);
    }
  }

  std::shared_ptr<Symbol> symbolFor(const Node *node) const {
    auto it = symbolsByNode.find(node);
    return it == symbolsByNode.end() ? nullptr : it->second;
  }

  std::shared_ptr<zir::Type> typeFor(const Node *node) const {
    auto it = typesByNode.find(node);
    return it == typesByNode.end() ? nullptr : it->second;
  }
};

} // namespace sema
