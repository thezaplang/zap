#pragma once
#include <map>
#include <string>
#include <memory>
#include <vector>
#include "symbol.hpp"

namespace sema {

class SymbolTable {
public:
  SymbolTable(std::shared_ptr<SymbolTable> parent = nullptr) : parent_(std::move(parent)) {}

  bool declare(const std::string& name, std::shared_ptr<Symbol> symbol) {
    if (symbols_.find(name) != symbols_.end()) {
      return false; // Already declared in this scope
    }
    symbols_[name] = std::move(symbol);
    return true;
  }

  std::shared_ptr<Symbol> lookup(const std::string& name) const {
    auto it = symbols_.find(name);
    if (it != symbols_.end()) {
      return it->second;
    }
    if (parent_) {
      return parent_->lookup(name);
    }
    return nullptr;
  }

  std::shared_ptr<SymbolTable> getParent() const { return parent_; }

private:
  std::shared_ptr<SymbolTable> parent_;
  std::map<std::string, std::shared_ptr<Symbol>> symbols_;
};

} // namespace sema
