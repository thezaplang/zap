#pragma once

#include "../binding_kind.hpp"
#include "../ir/type.hpp"
#include "../visibility.hpp"
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sema {

enum class SymbolKind { Variable, Function, OverloadSet, Type, Module };

class BoundExpression;

class Symbol {
public:
  std::string name;
  std::string linkName;
  std::string moduleName;
  std::shared_ptr<zir::Type> type;
  Visibility visibility = Visibility::Private;

  // Built-in attribute semantic flags (MVP).
  bool isErrorType = false; // @error on enum/struct
  bool hasReprC = false;    // @repr("C") on enum/struct
  bool isPacked = false;    // @packed on record-like types
  bool hasExternC = false;  // @extern("C") on function
  bool hasNoMangle = false; // @noMangle on function

  std::string reprValue; // e.g. "C"
  std::string externAbi; // e.g. "C"

  virtual ~Symbol() noexcept = default;
  virtual SymbolKind getKind() const noexcept = 0;

protected:
  Symbol(std::string n, std::shared_ptr<zir::Type> t, std::string link = "",
         std::string module = "", Visibility vis = Visibility::Private)
      : name(std::move(n)), linkName(link.empty() ? name : std::move(link)),
        moduleName(std::move(module)), type(std::move(t)), visibility(vis) {}
};

class VariableSymbol : public Symbol {
public:
  BindingKind binding_kind = BindingKind::Mutable;
  bool is_ref = false;
  bool is_sink = false;
  bool is_noescape = false;
  bool is_variadic_pack = false;
  bool is_external = false;
  bool is_global = false;
  std::shared_ptr<zir::Type> variadic_element_type = nullptr;
  std::shared_ptr<BoundExpression> constant_value = nullptr;
  VariableSymbol(std::string n, std::shared_ptr<zir::Type> t,
                 BindingKind kind = BindingKind::Mutable, bool isRef = false,
                 std::string link = "", std::string module = "",
                 Visibility vis = Visibility::Private)
      : Symbol(std::move(n), std::move(t), std::move(link), std::move(module),
               vis),
        binding_kind(kind), is_ref(isRef) {}
  SymbolKind getKind() const noexcept override { return SymbolKind::Variable; }

  bool isMutableBinding() const noexcept {
    return binding_kind == BindingKind::Mutable;
  }

  bool isImmutableBinding() const noexcept {
    return binding_kind == BindingKind::Immutable;
  }

  bool isCompileTimeConstant() const noexcept {
    return binding_kind == BindingKind::CompileTimeConstant;
  }
};

class FunctionSymbol : public Symbol {
public:
  std::vector<std::shared_ptr<VariableSymbol>> parameters;
  std::shared_ptr<zir::Type> returnType;
  std::vector<std::string> genericParameterNames;
  bool isGenericInstantiation = false;
  std::map<std::string, std::shared_ptr<zir::Type>> genericArguments;
  bool isUnsafe = false;
  bool isCVariadic = false;
  bool isMethod = false;
  bool isStatic = false;
  bool isConstructor = false;
  bool isDestructor = false;
  bool isExternal = false;
  bool isEntryModule = false;
  bool returnsRef = false;
  bool hasEntry = false; // @entry on a callable function
  zir::ResultBorrowContract resultBorrow;
  int vtableSlot = -1;
  std::string ownerTypeCodegenName;

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

class OverloadSetSymbol : public Symbol {
public:
  std::vector<std::shared_ptr<FunctionSymbol>> overloads;

  explicit OverloadSetSymbol(std::string n, std::string module = "",
                             Visibility vis = Visibility::Private)
      : Symbol(std::move(n), nullptr, "", std::move(module), vis) {}

  SymbolKind getKind() const noexcept override {
    return SymbolKind::OverloadSet;
  }

  bool addOverload(std::shared_ptr<FunctionSymbol> function) {
    if (!function) {
      return false;
    }
    overloads.push_back(std::move(function));
    return true;
  }
};

class TypeSymbol : public Symbol {
public:
  std::vector<std::string> genericParameterNames;
  bool isGenericInstantiation = false;
  std::map<std::string, std::shared_ptr<zir::Type>> genericArguments;
  bool isUnsafe = false;
  bool isClass = false;
  bool isInterface = false;
  TypeSymbol(std::string n, std::shared_ptr<zir::Type> t, std::string link = "",
             std::string module = "", Visibility vis = Visibility::Private,
             bool unsafe = false, bool classType = false)
      : Symbol(std::move(n), std::move(t), std::move(link), std::move(module),
               vis),
        isUnsafe(unsafe), isClass(classType) {}
  SymbolKind getKind() const noexcept override { return SymbolKind::Type; }
};

class ModuleSymbol : public Symbol {
public:
  std::map<std::string, std::shared_ptr<Symbol>> members;
  std::map<std::string, std::shared_ptr<Symbol>> exports;

  explicit ModuleSymbol(std::string n, std::string module = "")
      : Symbol(std::move(n), nullptr, "", std::move(module),
               Visibility::Public) {}

  SymbolKind getKind() const noexcept override { return SymbolKind::Module; }
};

} // namespace sema
