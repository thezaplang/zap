#pragma once
#include <string>
#include <memory>
#include <vector>
#include "../ir/type.hpp"

namespace sema {

enum class SymbolKind { Variable, Function, Type };

class Symbol {
public:
  std::string name;
  std::shared_ptr<zir::Type> type;
  virtual ~Symbol() = default;
  virtual SymbolKind getKind() const = 0;
  
protected:
  Symbol(std::string n, std::shared_ptr<zir::Type> t) : name(std::move(n)), type(std::move(t)) {}
};

class VariableSymbol : public Symbol {
public:
  VariableSymbol(std::string n, std::shared_ptr<zir::Type> t) : Symbol(std::move(n), std::move(t)) {}
  SymbolKind getKind() const override { return SymbolKind::Variable; }
};

class FunctionSymbol : public Symbol {
public:
  std::vector<std::shared_ptr<VariableSymbol>> parameters;
  std::shared_ptr<zir::Type> returnType;

  FunctionSymbol(std::string n, std::vector<std::shared_ptr<VariableSymbol>> params, std::shared_ptr<zir::Type> retType)
      : Symbol(std::move(n), nullptr), parameters(std::move(params)), returnType(std::move(retType)) {}
  
  SymbolKind getKind() const override { return SymbolKind::Function; }
};

class TypeSymbol : public Symbol {
public:
  TypeSymbol(std::string n, std::shared_ptr<zir::Type> t) : Symbol(std::move(n), std::move(t)) {}
  SymbolKind getKind() const override { return SymbolKind::Type; }
};

} // namespace sema
