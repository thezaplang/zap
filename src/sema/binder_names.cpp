#include "binder.hpp"

namespace sema {

void Binder::recordModuleMemberName(ExpressionNode &expression,
                                    const std::shared_ptr<Symbol> &symbol) {
  if (!semanticInfo_)
    return;
  ExpressionNode *first = &expression;
  size_t components = 1;
  while (auto member = dynamic_cast<MemberAccessNode *>(first)) {
    ++components;
    first = member->left_.get();
  }
  if (auto identifier = dynamic_cast<ConstId *>(first))
    semanticInfo_->recordName(identifier->syntaxName_, symbol, components);
}

std::shared_ptr<Symbol>
Binder::lookupVisibleSymbol(const std::string &name) const {
  return currentScope_ ? currentScope_->lookup(name) : nullptr;
}

std::shared_ptr<Symbol> Binder::lookupSyntaxName(const SyntaxName &name) const {
  if (!currentScope_)
    return nullptr;
  auto symbol = currentScope_->lookup(name);
  if (!symbol && name.context != ROOT_SYNTAX_CONTEXT &&
      name.context == name.expansionMark && !name.definitionModuleId.empty()) {
    auto module = modules_.find(name.definitionModuleId);
    if (module != modules_.end())
      symbol = module->second.scope->lookup(name.text);
  }
  if (semanticInfo_)
    semanticInfo_->recordName(name, symbol);
  return symbol;
}

std::shared_ptr<Symbol>
Binder::resolveModuleMember(const std::string &moduleName,
                            const std::string &memberName, SourceSpan span) {
  auto moduleSym = std::dynamic_pointer_cast<ModuleSymbol>(
      currentScope_->lookup(moduleName));
  if (!moduleSym) {
    error(span, "Undefined module: " + moduleName);
    return nullptr;
  }

  auto exportedIt = moduleSym->exports.find(memberName);
  if (exportedIt != moduleSym->exports.end()) {
    return exportedIt->second;
  }

  auto memberIt = moduleSym->members.find(memberName);
  if (memberIt == moduleSym->members.end()) {
    error(span,
          "Module '" + moduleName + "' has no member '" + memberName + "'.");
    return nullptr;
  }
  error(span, "Member '" + memberName + "' of module '" + moduleName +
                  "' is private.");
  return nullptr;
}

std::shared_ptr<Symbol>
Binder::resolveQualifiedSymbol(const std::vector<std::string> &parts,
                               SourceSpan span, SymbolKind expectedKind,
                               bool allowAnyKind, const SyntaxName *firstName) {
  if (parts.empty()) {
    return nullptr;
  }

  auto symbol = firstName && firstName->text == parts.front()
                    ? lookupSyntaxName(*firstName)
                    : lookupVisibleSymbol(parts.front());
  if (!symbol) {
    error(span, "Undefined identifier: " + parts.front());
    return nullptr;
  }

  for (size_t i = 1; i < parts.size(); ++i) {
    auto moduleSym = std::dynamic_pointer_cast<ModuleSymbol>(symbol);
    if (!moduleSym) {
      error(span, "'" + parts[i - 1] + "' is not a module.");
      return nullptr;
    }

    auto memberIt = moduleSym->exports.find(parts[i]);
    if (memberIt == moduleSym->exports.end()) {
      auto privateIt = moduleSym->members.find(parts[i]);
      if (privateIt != moduleSym->members.end()) {
        error(span, "Member '" + parts[i] + "' of module '" + moduleSym->name +
                        "' is private.");
      } else {
        error(span, "Module '" + moduleSym->name + "' has no member '" +
                        parts[i] + "'.");
      }
      return nullptr;
    }

    symbol = memberIt->second;
  }

  if (symbol->getKind() == SymbolKind::CompileTimeFunction) {
    error(span, "Cannot use @ctfe function '" + symbol->name + "' at runtime.");
    return nullptr;
  }
  if (!allowAnyKind && symbol->getKind() != expectedKind &&
      !(expectedKind == SymbolKind::Function &&
        symbol->getKind() == SymbolKind::OverloadSet)) {
    return nullptr;
  }
  if (semanticInfo_ && firstName)
    semanticInfo_->recordName(*firstName, symbol, parts.size());
  return symbol;
}

} // namespace sema
