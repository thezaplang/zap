#pragma once
#include "symbol.hpp"
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sema {

class SymbolTable {
public:
  SymbolTable(std::shared_ptr<SymbolTable> parent = nullptr)
      : parent_(std::move(parent)) {}

  bool declare(const std::string &name, std::shared_ptr<Symbol> symbol) {
    if (symbols_.find(name) != symbols_.end()) {
      return false; // Already declared in this scope
    }
    symbols_[name] = std::move(symbol);
    return true;
  }

  std::shared_ptr<OverloadSetSymbol>
  declareFunction(const std::string &name,
                  std::shared_ptr<FunctionSymbol> function) {
    auto it = symbols_.find(name);
    if (it == symbols_.end()) {
      auto set = std::make_shared<OverloadSetSymbol>(
          name, function ? function->moduleName : "");
      set->visibility = function ? function->visibility : Visibility::Private;
      if (function) {
        set->addOverload(function);
      }
      symbols_[name] = set;
      return set;
    }

    auto set = std::dynamic_pointer_cast<OverloadSetSymbol>(it->second);
    if (!set) {
      return nullptr;
    }
    if (function) {
      set->addOverload(function);
    }
    return set;
  }

  std::shared_ptr<Symbol> lookup(const std::string &name) const {
    auto it = symbols_.find(name);
    if (it != symbols_.end()) {
      return it->second;
    }
    if (parent_) {
      return parent_->lookup(name);
    }
    return nullptr;
  }

  std::shared_ptr<Symbol> lookupLocal(const std::string &name) const {
    auto it = symbols_.find(name);
    if (it != symbols_.end()) {
      return it->second;
    }
    return nullptr;
  }

  std::shared_ptr<SymbolTable> getParent() const { return parent_; }

private:
  std::shared_ptr<SymbolTable> parent_;
  std::map<std::string, std::shared_ptr<Symbol>> symbols_;
};

} // namespace sema
