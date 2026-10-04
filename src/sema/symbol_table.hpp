#pragma once
#include "../token/syntax_name.hpp"
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

    bool declare(const std::string& name, std::shared_ptr<Symbol> symbol) {
        return declare(SyntaxName(name), std::move(symbol));
    }

    bool declare(const SyntaxName& name, std::shared_ptr<Symbol> symbol) {
        const Key key{name.text, name.context};
        if (symbols_.find(key) != symbols_.end()) {
            return false; // Already declared in this scope
        }
        symbols_[key] = std::move(symbol);
        return true;
    }

    std::shared_ptr<OverloadSetSymbol> declareFunction(
        const std::string& name,
        std::shared_ptr<FunctionSymbol> function
    ) {
        return declareFunction(SyntaxName(name), std::move(function));
    }

    std::shared_ptr<OverloadSetSymbol> declareFunction(
        const SyntaxName& name,
        std::shared_ptr<FunctionSymbol> function
    ) {
        const Key key{name.text, name.context};
        auto it = symbols_.find(key);
        if (it == symbols_.end()) {
            auto set = std::make_shared<OverloadSetSymbol>(
                name.text,
                function ? function->moduleName : ""
            );
            set->visibility = function ? function->visibility : Visibility::Private;
            if (function) {
                set->addOverload(function);
            }
            symbols_[key] = set;
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

    std::shared_ptr<Symbol> lookup(const std::string& name) const {
        return lookup(SyntaxName(name));
    }

    std::shared_ptr<Symbol> lookup(const SyntaxName& name) const {
        auto it = symbols_.find(Key{name.text, name.context});
        if (it != symbols_.end()) {
            return it->second;
        }
        if (parent_) {
            return parent_->lookup(name);
        }
        return nullptr;
    }

    std::shared_ptr<Symbol> lookupLocal(const std::string& name) const {
        return lookupLocal(SyntaxName(name));
    }

    std::shared_ptr<Symbol> lookupLocal(const SyntaxName& name) const {
        auto it = symbols_.find(Key{name.text, name.context});
        if (it != symbols_.end()) {
            return it->second;
        }
        return nullptr;
    }

    std::shared_ptr<SymbolTable> getParent() const { return parent_; }

private:
    using Key = std::pair<std::string, SyntaxContextId>;
    std::shared_ptr<SymbolTable> parent_;
    std::map<Key, std::shared_ptr<Symbol>> symbols_;
};

} // namespace sema
