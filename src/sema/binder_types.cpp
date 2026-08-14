#include "../ast/class_decl.hpp"
#include "../ast/const/const_char.hpp"
#include "../ast/record_decl.hpp"
#include "../ir/string_type.hpp"
#include "binder.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <functional>
#include <limits>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace sema {

bool Binder::isSupportedBuiltInAttribute(const std::string &name) const {
  static const std::unordered_set<std::string> builtIns = {
      "error", "repr", "packed", "extern", "noMangle", "entry"};
  return builtIns.find(name) != builtIns.end();
}

void Binder::warnUnknownAttributes(const TopLevel &node) {
  for (const auto &attr : node.attributes_) {
    if (!isSupportedBuiltInAttribute(attr.name)) {
      _diag.report(attr.span, zap::DiagnosticLevel::Warning,
                   "unknown attribute '" + attr.name + "'");
    }
  }
}

void Binder::validateAndApplyTypeAttributes(
    const TopLevel &node, const std::shared_ptr<TypeSymbol> &symbol,
    bool allowErrorAttribute) {
  if (!symbol) {
    return;
  }

  warnUnknownAttributes(node);

  bool seenError = false;
  bool seenRepr = false;
  bool seenPacked = false;

  for (const auto &attr : node.attributes_) {
    if (attr.name == "error") {
      if (!allowErrorAttribute) {
        error(attr.span, "attribute 'error' cannot be applied to this type");
        continue;
      }
      if (seenError) {
        error(attr.span, "duplicate attribute 'error'");
        continue;
      }
      if (attr.hasArguments()) {
        error(attr.span, "attribute 'error' does not accept arguments");
        continue;
      }
      seenError = true;
      symbol->isErrorType = true;
      continue;
    }

    if (attr.name == "repr") {
      if (seenRepr) {
        error(attr.span, "duplicate attribute 'repr'");
        continue;
      }
      seenRepr = true;

      if (attr.arguments.size() != 1 ||
          attr.arguments[0].kind != AttributeArgumentKind::Positional) {
        error(
            attr.span,
            "attribute 'repr' expects exactly one positional string argument");
        continue;
      }

      auto *str = dynamic_cast<ConstString *>(attr.arguments[0].value.get());
      if (!str) {
        error(attr.span, "attribute 'repr' expects a string literal argument");
        continue;
      }

      if (str->value_ != "C") {
        error(attr.span,
              "invalid argument for attribute 'repr': expected \"C\"");
        continue;
      }

      symbol->hasReprC = true;
      symbol->reprValue = str->value_;
      continue;
    }

    if (attr.name == "packed") {
      if (seenPacked) {
        error(attr.span, "duplicate attribute 'packed'");
        continue;
      }
      if (attr.hasArguments()) {
        error(attr.span, "attribute 'packed' does not accept arguments");
        continue;
      }
      auto recordType =
          std::dynamic_pointer_cast<zir::RecordType>(symbol->type);
      if (!recordType || symbol->type->getKind() != zir::TypeKind::Record) {
        error(attr.span,
              "attribute 'packed' can only be applied to record types");
        continue;
      }
      seenPacked = true;
      symbol->isPacked = true;
      recordType->isPacked = true;
      continue;
    }

    if (attr.name == "extern" || attr.name == "noMangle" ||
        attr.name == "entry") {
      error(attr.span, "attribute '" + attr.name +
                           "' cannot be applied to type declarations");
      continue;
    }
  }
}

void Binder::validateAndApplyFunctionAttributes(
    const TopLevel &node, const std::shared_ptr<FunctionSymbol> &symbol,
    bool isExternalDeclaration) {
  (void)isExternalDeclaration;
  if (!symbol) {
    return;
  }

  warnUnknownAttributes(node);

  bool seenExtern = false;
  bool seenNoMangle = false;
  bool seenEntry = false;

  for (const auto &attr : node.attributes_) {
    if (attr.name == "extern") {
      if (seenExtern) {
        error(attr.span, "duplicate attribute 'extern'");
        continue;
      }
      seenExtern = true;

      if (attr.arguments.size() != 1 ||
          attr.arguments[0].kind != AttributeArgumentKind::Positional) {
        error(attr.span, "attribute 'extern' expects exactly one positional "
                         "string argument");
        continue;
      }

      auto *str = dynamic_cast<ConstString *>(attr.arguments[0].value.get());
      if (!str) {
        error(attr.span,
              "attribute 'extern' expects a string literal argument");
        continue;
      }

      if (str->value_ != "C") {
        error(attr.span,
              "invalid argument for attribute 'extern': expected \"C\"");
        continue;
      }

      symbol->hasExternC = true;
      symbol->externAbi = str->value_;
      continue;
    }

    if (attr.name == "noMangle") {
      if (seenNoMangle) {
        error(attr.span, "duplicate attribute 'noMangle'");
        continue;
      }
      if (attr.hasArguments()) {
        error(attr.span, "attribute 'noMangle' does not accept arguments");
        continue;
      }
      seenNoMangle = true;
      symbol->hasNoMangle = true;
      continue;
    }

    if (attr.name == "entry") {
      if (seenEntry) {
        error(attr.span, "duplicate attribute 'entry'");
        continue;
      }
      if (attr.hasArguments()) {
        error(attr.span, "attribute 'entry' does not accept arguments");
        continue;
      }
      if (isExternalDeclaration || symbol->isConstructor ||
          symbol->isDestructor || (symbol->isMethod && !symbol->isStatic) ||
          (!symbol->genericParameterNames.empty() &&
           !symbol->isGenericInstantiation)) {
        error(attr.span, "attribute 'entry' can only be applied to non-generic "
                         "functions with a body or static methods");
        continue;
      }
      seenEntry = true;
      symbol->hasEntry = true;
      continue;
    }

    if (attr.name == "repr" || attr.name == "error" || attr.name == "packed") {
      error(attr.span,
            "attribute '" + attr.name + "' cannot be applied to functions");
      continue;
    }
  }
}

std::shared_ptr<zir::Type> Binder::mapType(const TypeNode &typeNode) {
  // Cache simple types when not in a generic instantiation context.
  // Skip cache for array types with size expressions (they evaluate AST nodes)
  // and for failable types (they may emit errors on first call).
  bool canCache = activeGenericBindingsStack_.empty() && !typeNode.isVarArgs &&
                  !typeNode.isFailable &&
                  !(typeNode.isArray && typeNode.arraySize);
  if (canCache) {
    auto it = mapTypeCache_.find(&typeNode);
    if (it != mapTypeCache_.end())
      return it->second;
  }

  auto doMap = [&]() -> std::shared_ptr<zir::Type> {
    if (typeNode.isVarArgs) {
      if (!typeNode.baseType)
        return nullptr;
      return mapType(*typeNode.baseType);
    }

    if (typeNode.isFailable) {
      if (!typeNode.baseType || !typeNode.errorType) {
        error(typeNode.span, "Invalid failable type declaration.");
        return nullptr;
      }

      auto valueType = mapType(*typeNode.baseType);
      auto errorType = mapType(*typeNode.errorType);

      if (!valueType || !errorType) {
        return nullptr;
      }

      if (!typeNode.errorType->qualifiers.empty() ||
          !typeNode.errorType->typeName.empty()) {
        std::vector<std::string> errParts = typeNode.errorType->qualifiers;
        errParts.push_back(typeNode.errorType->typeName);
        auto errSymbol = resolveQualifiedSymbol(
            errParts, typeNode.errorType->span, SymbolKind::Type);
        if (errSymbol && errSymbol->getKind() == SymbolKind::Type) {
          auto errTypeSymbol = std::static_pointer_cast<TypeSymbol>(errSymbol);
          if (!errTypeSymbol->isErrorType) {
            error(typeNode.errorType->span,
                  "Type '" + typeNode.errorType->qualifiedName() +
                      "' used as failable error type must be annotated with "
                      "@error.");
          }
        }
      }

      return makeFailableType(valueType, errorType);
    }

    if (!activeGenericBindingsStack_.empty() && typeNode.qualifiers.empty() &&
        typeNode.genericArgs.empty()) {
      const auto &bindings = activeGenericBindingsStack_.back();
      auto it = bindings.find(typeNode.typeName);
      if (it != bindings.end()) {
        auto mapped = it->second;
        if (typeNode.isWeak) {
          if (!mapped || mapped->getKind() != zir::TypeKind::Class) {
            error(typeNode.span, "'weak' can only be used with class types.");
            return nullptr;
          }
          auto weakType = std::make_shared<zir::ClassType>(
              *std::static_pointer_cast<zir::ClassType>(mapped));
          weakType->setWeak(true);
          return weakType;
        }
        return mapped;
      }
    }

    if (typeNode.isArray) {
      if (!typeNode.baseType)
        return nullptr;
      auto base = mapType(*typeNode.baseType);

      if (!typeNode.arraySize) {
        return makeVariadicViewType(base);
      }

      size_t size = 0;
      typeNode.arraySize->accept(*this);
      if (!expressionStack_.empty()) {
        auto boundSize = std::move(expressionStack_.top());
        expressionStack_.pop();
        auto evaluated = evaluateConstantInt(boundSize.get());
        if (evaluated) {
          size = static_cast<size_t>(*evaluated);
        } else {
          error(typeNode.span,
                "Array size must be a constant integer expression.");
        }
      }

      return std::make_shared<zir::ArrayType>(std::move(base), size);
    }

    if (typeNode.isPointer) {
      if (!typeNode.baseType)
        return nullptr;
      auto base = mapType(*typeNode.baseType);
      return std::make_shared<zir::PointerType>(std::move(base));
    }

    if (typeNode.isFunPtr) {
      std::vector<std::shared_ptr<zir::Type>> params;
      for (const auto &p : typeNode.funPtrParams) {
        auto mapped = mapType(*p);
        if (!mapped)
          return nullptr;
        params.push_back(std::move(mapped));
      }
      auto ret =
          typeNode.funPtrReturn
              ? mapType(*typeNode.funPtrReturn)
              : std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
      if (!ret)
        return nullptr;
      std::vector<zir::ParameterOwnership> ownership;
      std::vector<zir::ParameterEscape> escape;
      ownership.reserve(params.size());
      escape.reserve(params.size());
      for (size_t i = 0; i < params.size(); ++i) {
        if (!zir::containsManagedValues(params[i])) {
          ownership.push_back(zir::ParameterOwnership::Borrow);
        } else if (i < typeNode.funPtrParamSinks.size() &&
                   typeNode.funPtrParamSinks[i]) {
          ownership.push_back(zir::ParameterOwnership::Sink);
        } else {
          ownership.push_back(zir::ParameterOwnership::Transfer);
        }
        const bool noescape = i < typeNode.funPtrParamNoEscapes.size() &&
                              typeNode.funPtrParamNoEscapes[i];
        if (noescape && ownership.back() != zir::ParameterOwnership::Borrow) {
          error(typeNode.span,
                "A transferring function pointer parameter cannot have a "
                "'noescape' contract.");
        }
        escape.push_back(noescape ? zir::ParameterEscape::NoEscape
                                  : zir::ParameterEscape::Unspecified);
      }
      zir::ResultBorrowContract resultBorrow;
      if (typeNode.funPtrResultBorrowSource) {
        const auto &source = *typeNode.funPtrResultBorrowSource;
        const bool numeric =
            !source.empty() &&
            std::all_of(source.begin(), source.end(),
                        [](unsigned char c) { return std::isdigit(c) != 0; });
        size_t sourceIndex = params.size();
        if (numeric) {
          try {
            sourceIndex = static_cast<size_t>(std::stoull(source));
          } catch (const std::exception &) {
            sourceIndex = params.size();
          }
        }
        if (!numeric || sourceIndex >= params.size()) {
          error(typeNode.span,
                "Function pointer 'borrows' source must be a valid parameter "
                "index.");
        } else if (typeNode.funPtrReturnsRef ||
                   ret->getIntrinsicKind() !=
                   zir::IntrinsicTypeKind::StringView) {
          error(typeNode.span,
                "Function pointer 'borrows' requires a StringView result.");
        } else if (params[sourceIndex]->getIntrinsicKind() !=
                   zir::IntrinsicTypeKind::StringView) {
          error(typeNode.span,
                "Function pointer 'borrows' currently requires a StringView "
                "source parameter.");
        } else if (escape[sourceIndex] == zir::ParameterEscape::NoEscape) {
          error(typeNode.span,
                "A noescape function pointer parameter cannot back the "
                "result.");
        } else {
          resultBorrow =
              zir::ResultBorrowContract::fromParameter(sourceIndex);
        }
      }
      return std::make_shared<zir::FunctionPointerType>(
          std::move(params), std::move(ret), std::move(ownership),
          std::move(escape), resultBorrow, typeNode.funPtrReturnsRef);
    }

    std::vector<std::string> parts = typeNode.qualifiers;
    parts.push_back(typeNode.typeName);
    auto symbol =
        resolveQualifiedSymbol(parts, typeNode.span, SymbolKind::Type);
    if (symbol && symbol->getKind() == SymbolKind::Type) {
      auto typeSymbol = std::static_pointer_cast<TypeSymbol>(symbol);
      if (!typeSymbol->genericParameterNames.empty()) {
        typeSymbol = instantiateGenericTypeSymbol(typeSymbol, typeNode);
        if (!typeSymbol) {
          return nullptr;
        }
      } else if (!typeNode.genericArgs.empty()) {
        error(typeNode.span,
              "Type '" + typeNode.qualifiedName() + "' is not generic.");
        return nullptr;
      }
      if (typeSymbol->isUnsafe) {
        requireUnsafeContext(typeNode.span, "unsafe struct types");
      }
      if (typeNode.isWeak) {
        if (!typeSymbol->isClass ||
            typeSymbol->type->getKind() != zir::TypeKind::Class) {
          error(typeNode.span, "'weak' can only be used with class types.");
          return nullptr;
        }
        auto weakType = std::make_shared<zir::ClassType>(
            *std::static_pointer_cast<zir::ClassType>(typeSymbol->type));
        weakType->setWeak(true);
        return weakType;
      }

      auto resolvedType = typeSymbol->type;
      if (typeNode.errorType && !typeSymbol->isErrorType) {
        error(typeNode.span,
              "Type '" + typeNode.qualifiedName() +
                  "' used as failable error type must be annotated with "
                  "@error.");
      }
      return resolvedType;
    }

    return nullptr;
  }; // end doMap lambda

  auto result = doMap();
  if (canCache)
    mapTypeCache_[&typeNode] = result;
  return result;
}

} // namespace sema
