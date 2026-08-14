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

std::shared_ptr<FunctionSymbol> Binder::ensureGenericFunctionInstantiation(
    const std::shared_ptr<FunctionSymbol> &baseFunction,
    const std::vector<std::pair<std::string, std::shared_ptr<zir::Type>>>
        &genericBindings,
    SourceSpan callSpan) {
  if (!baseFunction) {
    return nullptr;
  }

  if (baseFunction->genericParameterNames.empty()) {
    return baseFunction;
  }

  std::vector<std::string> missing;
  for (const auto &name : baseFunction->genericParameterNames) {
    auto it =
        std::find_if(genericBindings.begin(), genericBindings.end(),
                     [&](const auto &entry) { return entry.first == name; });
    if (it == genericBindings.end()) {
      missing.push_back(name);
    }
  }
  if (!missing.empty()) {
    std::string msg;
    msg.reserve(64);
    msg += "Missing generic type arguments for function '";
    msg += baseFunction->name;
    msg += "': ";
    for (size_t i = 0; i < missing.size(); ++i) {
      if (i != 0)
        msg += ", ";
      msg += missing[i];
    }
    error(callSpan, msg);
    return nullptr;
  }

  std::string cacheKey;
  {
    size_t estimatedSize = baseFunction->linkName.size() + 2 +
                           baseFunction->genericParameterNames.size() * 16;
    cacheKey.reserve(estimatedSize);
    cacheKey += baseFunction->linkName;
    cacheKey += '<';
    for (size_t i = 0; i < baseFunction->genericParameterNames.size(); ++i) {
      if (i != 0)
        cacheKey += ',';
      const auto &name = baseFunction->genericParameterNames[i];
      auto it =
          std::find_if(genericBindings.begin(), genericBindings.end(),
                       [&](const auto &entry) { return entry.first == name; });
      cacheKey += name;
      cacheKey += '=';
      cacheKey += (it != genericBindings.end() && it->second)
                      ? typeInterner_.mangleKey(it->second)
                      : "missing";
    }
    cacheKey += '>';
  }

  auto cachedIt = genericFunctionInstantiations_.find(cacheKey);
  if (cachedIt != genericFunctionInstantiations_.end()) {
    return cachedIt->second;
  }

  auto declIt = functionDeclarationNodes_.find(baseFunction.get());
  if (declIt == functionDeclarationNodes_.end() || !declIt->second) {
    error(callSpan,
          "Internal error: missing declaration for generic function '" +
              baseFunction->name + "'.");
    return nullptr;
  }

  auto moduleIdIt = functionDeclarationModuleIds_.find(baseFunction.get());
  auto moduleId = moduleIdIt == functionDeclarationModuleIds_.end()
                      ? currentModuleId_
                      : moduleIdIt->second;
  auto moduleIt = modules_.find(moduleId);
  if (moduleIt == modules_.end() || !moduleIt->second.info) {
    error(
        callSpan,
        "Internal error: current module not found for generic instantiation.");
    return nullptr;
  }

  std::unordered_map<std::string, std::shared_ptr<zir::Type>> genericBindingMap;
  genericBindingMap.reserve(genericBindings.size());
  for (const auto &[name, type] : genericBindings) {
    genericBindingMap[name] = type;
  }

  std::vector<std::shared_ptr<VariableSymbol>> instantiatedParams;
  instantiatedParams.reserve(baseFunction->parameters.size());
  for (const auto &param : baseFunction->parameters) {
    auto instType = substituteGenericType(param->type, genericBindingMap);
    auto instParam = std::make_shared<VariableSymbol>(
        param->name, instType, param->binding_kind, param->is_ref,
        param->linkName, param->moduleName, param->visibility);
    instParam->is_sink = param->is_sink;
    instParam->is_noescape = param->is_noescape;
    instParam->is_variadic_pack = param->is_variadic_pack;
    instParam->variadic_element_type =
        substituteGenericType(param->variadic_element_type, genericBindingMap);
    instantiatedParams.push_back(std::move(instParam));
  }

  auto instantiatedReturn =
      substituteGenericType(baseFunction->returnType, genericBindingMap);

  auto instantiated = std::make_shared<FunctionSymbol>(
      baseFunction->name, std::move(instantiatedParams), instantiatedReturn, "",
      baseFunction->moduleName, baseFunction->visibility,
      baseFunction->isUnsafe, baseFunction->isCVariadic);
  instantiated->isMethod = baseFunction->isMethod;
  instantiated->isStatic = baseFunction->isStatic;
  instantiated->isConstructor = baseFunction->isConstructor;
  instantiated->isDestructor = baseFunction->isDestructor;
  instantiated->isEntryModule = baseFunction->isEntryModule;
  instantiated->hasNoMangle = baseFunction->hasNoMangle;
  instantiated->hasEntry = baseFunction->hasEntry;
  instantiated->vtableSlot = -1;
  instantiated->ownerTypeCodegenName = baseFunction->ownerTypeCodegenName;
  instantiated->resultBorrow = baseFunction->resultBorrow;
  instantiated->isGenericInstantiation = true;
  instantiated->genericArguments.clear();
  for (const auto &[name, type] : genericBindings) {
    instantiated->genericArguments[name] = type;
  }

  std::string genericSuffix;
  for (size_t i = 0; i < baseFunction->genericParameterNames.size(); ++i) {
    if (i != 0) {
      genericSuffix += "$";
    }
    const auto &name = baseFunction->genericParameterNames[i];
    auto it =
        std::find_if(genericBindings.begin(), genericBindings.end(),
                     [&](const auto &entry) { return entry.first == name; });
    if (it == genericBindings.end() || !it->second) {
      error(callSpan, "Missing binding for generic parameter '" + name + "'.");
      return nullptr;
    }
    genericSuffix +=
        sanitizeTypeName(name) + "_" + typeInterner_.mangleKey(it->second);
  }
  instantiated->linkName = baseFunction->linkName + "$g$" + genericSuffix;

  genericFunctionInstantiations_[cacheKey] = instantiated;
  functionGenericParamNames_[instantiated.get()] = {};
  functionDeclarationNodes_[instantiated.get()] = declIt->second;
  functionDeclarationModuleIds_[instantiated.get()] = moduleId;

  auto inProgressIt =
      std::find(genericInstantiationInProgress_.begin(),
                genericInstantiationInProgress_.end(), cacheKey);
  if (inProgressIt != genericInstantiationInProgress_.end()) {
    return instantiated;
  }

  auto emittedIt = genericInstantiationEmitted_.find(instantiated.get());
  if (emittedIt != genericInstantiationEmitted_.end() && emittedIt->second) {
    return instantiated;
  }

  genericInstantiationInProgress_.push_back(cacheKey);

  auto oldScope = currentScope_;
  auto oldFunction = currentFunction_;
  auto oldModuleId = currentModuleId_;
  int oldUnsafeDepth = unsafeDepth_;

  currentModuleId_ = moduleIt->second.info->moduleId;
  currentScope_ = moduleIt->second.scope;
  currentFunction_ = instantiated;
  if (instantiated->isUnsafe) {
    ++unsafeDepth_;
  }

  pushScope();
  for (const auto &param : instantiated->parameters) {
    if (!currentScope_->declare(param->name, param)) {
      error(callSpan, "Parameter '" + param->name +
                          "' already declared in generic instantiation.");
    }
  }

  activeGenericBindingsStack_.push_back(
      std::unordered_map<std::string, std::shared_ptr<zir::Type>>(
          genericBindings.begin(), genericBindings.end()));
  auto boundBody = bindBody(declIt->second->body_.get(), false);
  activeGenericBindingsStack_.pop_back();

  popScope();

  currentScope_ = oldScope;
  currentFunction_ = oldFunction;
  currentModuleId_ = oldModuleId;
  unsafeDepth_ = oldUnsafeDepth;

  bool hasReturn = blockAlwaysReturns(boundBody.get());

  if (!hasReturn && instantiated->linkName == "main" &&
      instantiated->returnType->isInteger()) {
    auto intType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int);
    auto lit = std::make_unique<BoundLiteral>("0", intType);
    boundBody->statements.push_back(
        std::make_unique<BoundReturnStatement>(std::move(lit)));
    hasReturn = true;
  }

  if (!hasReturn &&
      instantiated->returnType->getKind() != zir::TypeKind::Void) {
    auto kind = instantiated->returnType->getKind();
    if (instantiated->returnType->isInteger() ||
        instantiated->returnType->isFloatingPoint() ||
        kind == zir::TypeKind::Bool) {
      std::string litVal = "0";
      if (instantiated->returnType->isFloatingPoint())
        litVal = "0.0";
      else if (kind == zir::TypeKind::Bool)
        litVal = "false";
      auto lit =
          std::make_unique<BoundLiteral>(litVal, instantiated->returnType);
      boundBody->statements.push_back(
          std::make_unique<BoundReturnStatement>(std::move(lit)));
    }

    _diag.report(callSpan, zap::DiagnosticLevel::Warning,
                 "Generic function '" + instantiated->name +
                     "' has non-void return type but no return on some paths.");
  }

  boundRoot_->functions.push_back(std::make_unique<BoundFunctionDeclaration>(
      instantiated, std::move(boundBody)));
  genericInstantiationEmitted_[instantiated.get()] = true;

  genericInstantiationInProgress_.pop_back();
  return instantiated;
}

bool Binder::isGenericTypeParameterName(std::string_view name) const {
  if (activeGenericBindingsStack_.empty()) {
    return false;
  }
  const auto &bindings = activeGenericBindingsStack_.back();
  return bindings.find(std::string(name)) != bindings.end();
}

std::shared_ptr<zir::Type> Binder::substituteGenericType(
    std::shared_ptr<zir::Type> type,
    const std::unordered_map<std::string, std::shared_ptr<zir::Type>>
        &genericBindings) const {
  if (!type) {
    return nullptr;
  }

  if (type->getKind() == zir::TypeKind::Record) {
    auto record = std::static_pointer_cast<zir::RecordType>(type);
    if (record->getRole() == zir::RecordRole::GenericParameter) {
      auto it = genericBindings.find(record->getName());
      return it != genericBindings.end() ? it->second : type;
    }
    if (record->getRole() == zir::RecordRole::VariadicView) {
      const auto &fields = record->getFields();
      if (fields.size() >= 2 &&
          fields[0].type->getKind() == zir::TypeKind::Pointer) {
        auto dataPtr =
            std::static_pointer_cast<zir::PointerType>(fields[0].type);
        return makeVariadicViewType(
            substituteGenericType(dataPtr->getBaseType(), genericBindings));
      }
      return type;
    }
    if (record->isGenericInstance()) {
      std::vector<std::shared_ptr<zir::Type>> substitutedArgs;
      substitutedArgs.reserve(record->getGenericArguments().size());
      for (const auto &arg : record->getGenericArguments()) {
        substitutedArgs.push_back(substituteGenericType(arg, genericBindings));
      }

      auto substituted = std::make_shared<zir::RecordType>(
          renderGenericTypeName(record->getGenericBaseName(), substitutedArgs),
          renderGenericCodegenName(record->getGenericCodegenBaseName(),
                                   substitutedArgs));
      substituted->setGenericInstance(record->getGenericBaseName(),
                                      record->getGenericCodegenBaseName(),
                                      substitutedArgs);
      for (const auto &field : record->getFields()) {
        substituted->addField(
            field.name, substituteGenericType(field.type, genericBindings),
            field.visibility);
      }
      return substituted;
    }

    return type;
  }

  if (type->getKind() == zir::TypeKind::Class) {
    auto classType = std::static_pointer_cast<zir::ClassType>(type);
    auto it = genericBindings.find(classType->getName());
    if (it != genericBindings.end()) {
      return it->second;
    }
    if (classType->isGenericInstance()) {
      std::vector<std::shared_ptr<zir::Type>> substitutedArgs;
      substitutedArgs.reserve(classType->getGenericArguments().size());
      for (const auto &arg : classType->getGenericArguments()) {
        substitutedArgs.push_back(substituteGenericType(arg, genericBindings));
      }

      auto substituted = std::make_shared<zir::ClassType>(
          renderGenericTypeName(classType->getGenericBaseName(),
                                substitutedArgs),
          renderGenericCodegenName(classType->getGenericCodegenBaseName(),
                                   substitutedArgs));
      substituted->setGenericInstance(classType->getGenericBaseName(),
                                      classType->getGenericCodegenBaseName(),
                                      substitutedArgs);
      substituted->setWeak(classType->isWeak());
      if (auto base = classType->getBase()) {
        auto substitutedBase = substituteGenericType(base, genericBindings);
        if (substitutedBase &&
            substitutedBase->getKind() == zir::TypeKind::Class) {
          substituted->setBase(
              std::static_pointer_cast<zir::ClassType>(substitutedBase));
        }
      }
      for (const auto &field : classType->getFields()) {
        substituted->addField(
            field.name, substituteGenericType(field.type, genericBindings),
            field.visibility);
      }
      return substituted;
    }
    return type;
  }

  if (type->getKind() == zir::TypeKind::Pointer) {
    auto ptr = std::static_pointer_cast<zir::PointerType>(type);
    auto base = substituteGenericType(ptr->getBaseType(), genericBindings);
    return std::make_shared<zir::PointerType>(base);
  }

  if (type->getKind() == zir::TypeKind::Array) {
    auto arr = std::static_pointer_cast<zir::ArrayType>(type);
    auto base = substituteGenericType(arr->getBaseType(), genericBindings);
    return std::make_shared<zir::ArrayType>(base, arr->getSize());
  }

  return type;
}

bool Binder::validateGenericConstraints(
    const std::vector<GenericConstraint> &constraints,
    std::unordered_map<std::string, std::shared_ptr<zir::Type>> &bindings,
    std::string *failureReason) {
  for (const auto &constraint : constraints) {
    auto boundIt = bindings.find(constraint.parameterName);
    if (boundIt == bindings.end() || !boundIt->second) {
      if (failureReason) {
        *failureReason = "missing binding for constrained type parameter '" +
                         constraint.parameterName + "'";
      }
      return false;
    }
    if (!constraint.boundType) {
      continue;
    }

    activeGenericBindingsStack_.push_back(bindings);
    auto requiredType = mapType(*constraint.boundType);
    activeGenericBindingsStack_.pop_back();
    if (!requiredType) {
      if (failureReason) {
        *failureReason =
            "unknown constraint type for '" + constraint.parameterName + "'";
      }
      return false;
    }
    if (!conversions_.isSubtype(boundIt->second, requiredType)) {
      if (failureReason) {
        *failureReason =
            "type parameter '" + constraint.parameterName + "' with type '" +
            boundIt->second->toString() + "' does not satisfy constraint '" +
            constraint.parameterName + ": " + requiredType->toString() + "'";
      }
      return false;
    }
  }
  return true;
}

std::vector<std::pair<std::string, std::shared_ptr<zir::Type>>>
Binder::orderedGenericBindings(
    const std::unordered_map<std::string, std::shared_ptr<zir::Type>>
        &genericBindings) const {
  std::vector<std::pair<std::string, std::shared_ptr<zir::Type>>> ordered;
  ordered.reserve(genericBindings.size());
  for (const auto &[name, type] : genericBindings) {
    ordered.emplace_back(name, type);
  }
  std::sort(
      ordered.begin(), ordered.end(),
      [](const auto &lhs, const auto &rhs) { return lhs.first < rhs.first; });
  return ordered;
}
} // namespace sema
