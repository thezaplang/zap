#pragma once
#include "../ir/type.hpp"
#include "../visibility.hpp"
#include <memory>
#include <map>
#include <string>
#include <vector>

namespace sema {

enum class SymbolKind { Variable, Function, Type, Module };

class BoundExpression;

class Symbol {
public:
  std::string name;
  std::string linkName;
  std::string moduleName;
  std::shared_ptr<zir::Type> type;
  Visibility visibility = Visibility::Private;
  virtual ~Symbol() noexcept = default;
  virtual SymbolKind getKind() const noexcept = 0;

protected:
  Symbol(std::string n, std::shared_ptr<zir::Type> t,
         std::string link = "", std::string module = "",
         Visibility vis = Visibility::Private)
      : name(std::move(n)), linkName(link.empty() ? name : std::move(link)),
        moduleName(std::move(module)), type(std::move(t)), visibility(vis) {}
};

class VariableSymbol : public Symbol {
public:
  bool is_const = false;
  bool is_ref = false;
  bool is_variadic_pack = false;
  std::shared_ptr<zir::Type> variadic_element_type = nullptr;
  std::shared_ptr<BoundExpression> constant_value = nullptr;
  VariableSymbol(std::string n, std::shared_ptr<zir::Type> t, bool isConst = false,
                 bool isRef = false, std::string link = "",
                 std::string module = "", Visibility vis = Visibility::Private)
      : Symbol(std::move(n), std::move(t), std::move(link), std::move(module), vis),
        is_const(isConst), is_ref(isRef) {}
  SymbolKind getKind() const noexcept override { return SymbolKind::Variable; }
};

class FunctionSymbol : public Symbol {
public:
  std::vector<std::shared_ptr<VariableSymbol>> parameters;
  std::shared_ptr<zir::Type> returnType;
  bool isUnsafe = false;
  bool isCVariadic = false;

  FunctionSymbol(std::string n,
                 std::vector<std::shared_ptr<VariableSymbol>> params,
                 std::shared_ptr<zir::Type> retType, std::string link = "",
                 std::string module = "", Visibility vis = Visibility::Private,
                 bool unsafe = false, bool cVariadic = false)
      : Symbol(std::move(n), nullptr, std::move(link), std::move(module), vis),
        parameters(std::move(params)), returnType(std::move(retType)),
        isUnsafe(unsafe), isCVariadic(cVariadic) {}

  SymbolKind getKind() const noexcept override { return SymbolKind::Function; }

  bool hasVariadicParameter() const {
    return !parameters.empty() && parameters.back()->is_variadic_pack;
  }

  bool acceptsExtraArguments() const {
    return hasVariadicParameter() || isCVariadic;
  }

  size_t fixedParameterCount() const {
    return hasVariadicParameter() ? parameters.size() - 1 : parameters.size();
  }

  std::shared_ptr<VariableSymbol> variadicParameter() const {
    return hasVariadicParameter() ? parameters.back() : nullptr;
  }
};

class TypeSymbol : public Symbol {
public:
  bool isUnsafe = false;
  TypeSymbol(std::string n, std::shared_ptr<zir::Type> t, std::string link = "",
             std::string module = "", Visibility vis = Visibility::Private,
             bool unsafe = false)
      : Symbol(std::move(n), std::move(t), std::move(link), std::move(module), vis),
        isUnsafe(unsafe) {}
  SymbolKind getKind() const noexcept override { return SymbolKind::Type; }
};

class ModuleSymbol : public Symbol {
public:
  std::map<std::string, std::shared_ptr<Symbol>> members;
  std::map<std::string, std::shared_ptr<Symbol>> exports;

  explicit ModuleSymbol(std::string n, std::string module = "")
      : Symbol(std::move(n), nullptr, "", std::move(module), Visibility::Public) {}

  SymbolKind getKind() const noexcept override { return SymbolKind::Module; }
};

} // namespace sema
