#include "../ast/class_decl.hpp"
#include "../ast/const/const_char.hpp"
#include "../ast/record_decl.hpp"
#include "../utils/string_type_utils.hpp"
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
                      ? it->second->toString()
                      : "<?>";
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
        param->name, instType, param->is_const, param->is_ref, param->linkName,
        param->moduleName, param->visibility);
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
  instantiated->vtableSlot = -1;
  instantiated->ownerTypeName = baseFunction->ownerTypeName;
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
    genericSuffix += sanitizeTypeName(name + "_" + abiTypeKey(it->second));
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
    if (instantiated->returnType->isInteger() || kind == zir::TypeKind::Float ||
        kind == zir::TypeKind::Bool) {
      std::string litVal = "0";
      if (kind == zir::TypeKind::Float)
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
    auto it = genericBindings.find(record->getName());
    if (it == genericBindings.end() && !record->getName().empty() &&
        record->getName()[0] == '%') {
      it = genericBindings.find(record->getName().substr(1));
    }
    if (it != genericBindings.end()) {
      return it->second;
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

    if (record->getName().rfind("__zap_varargs_", 0) == 0) {
      const auto &fields = record->getFields();
      if (fields.size() >= 2 &&
          fields[0].type->getKind() == zir::TypeKind::Pointer) {
        auto dataPtr =
            std::static_pointer_cast<zir::PointerType>(fields[0].type);
        auto elemType =
            substituteGenericType(dataPtr->getBaseType(), genericBindings);
        auto substitutedView = std::make_shared<zir::RecordType>(
            "__zap_varargs_" + sanitizeTypeName(elemType->toString()),
            "__zap_varargs_" + sanitizeTypeName(elemType->toString()));
        substitutedView->addField("data",
                                  std::make_shared<zir::PointerType>(elemType));
        substitutedView->addField(
            "len", std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int));
        return substitutedView;
      }
    }
    return type;
  }

  if (type->getKind() == zir::TypeKind::Class) {
    auto classType = std::static_pointer_cast<zir::ClassType>(type);
    auto it = genericBindings.find(classType->getName());
    if (it == genericBindings.end() && !classType->getName().empty() &&
        classType->getName()[0] == '%') {
      it = genericBindings.find(classType->getName().substr(1));
    }
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
    if (!canConvert(boundIt->second, requiredType)) {
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

std::shared_ptr<TypeSymbol> Binder::instantiateGenericTypeSymbol(
    const std::shared_ptr<TypeSymbol> &baseSymbol, const TypeNode &typeNode) {
  if (!baseSymbol || baseSymbol->genericParameterNames.empty()) {
    return baseSymbol;
  }

  std::string cacheKey = baseSymbol->linkName + "<";
  std::shared_ptr<zir::RecordType> instantiatedType;
  const RecordDecl *recordDecl = nullptr;
  const StructDeclarationNode *structDecl = nullptr;
  const ClassDecl *classDecl = nullptr;
  if (auto recordDeclIt = recordTypeDeclarationNodes_.find(baseSymbol.get());
      recordDeclIt != recordTypeDeclarationNodes_.end()) {
    recordDecl = recordDeclIt->second;
  } else if (auto structDeclIt =
                 structTypeDeclarationNodes_.find(baseSymbol.get());
             structDeclIt != structTypeDeclarationNodes_.end()) {
    structDecl = structDeclIt->second;
  } else if (auto classDeclIt =
                 classTypeDeclarationNodes_.find(baseSymbol.get());
             classDeclIt != classTypeDeclarationNodes_.end()) {
    classDecl = classDeclIt->second;
  } else {
    error(typeNode.span, "Generic type '" + typeNode.qualifiedName() +
                             "' is not instantiable yet.");
    return nullptr;
  }

  const std::vector<std::unique_ptr<TypeNode>> *declGenericParams =
      recordDecl ? &recordDecl->genericParams_
                 : (structDecl ? &structDecl->genericParams_
                               : &classDecl->genericParams_);
  const std::vector<GenericConstraint> *declGenericConstraints =
      recordDecl ? &recordDecl->genericConstraints_
                 : (structDecl ? &structDecl->genericConstraints_
                               : &classDecl->genericConstraints_);

  if (typeNode.genericArgs.size() > baseSymbol->genericParameterNames.size()) {
    error(typeNode.span, "Generic argument count mismatch for type '" +
                             typeNode.qualifiedName() + "'.");
    return nullptr;
  }

  std::vector<std::shared_ptr<zir::Type>> genericArgs;
  genericArgs.reserve(baseSymbol->genericParameterNames.size());
  std::unordered_map<std::string, std::shared_ptr<zir::Type>> genericBindings;
  for (size_t i = 0; i < baseSymbol->genericParameterNames.size(); ++i) {
    std::shared_ptr<zir::Type> mapped;
    if (i < typeNode.genericArgs.size()) {
      mapped = mapType(*typeNode.genericArgs[i]);
      if (!mapped) {
        error(typeNode.genericArgs[i]->span,
              "Unknown generic type argument in type '" +
                  typeNode.qualifiedName() + "'.");
        return nullptr;
      }
    } else {
      if (!activeGenericBindingsStack_.empty()) {
        const auto &stackBindings = activeGenericBindingsStack_.back();
        auto stackIt = stackBindings.find(baseSymbol->genericParameterNames[i]);
        if (stackIt != stackBindings.end()) {
          mapped = stackIt->second;
        }
      }
      if (!mapped) {
        const auto &declParam = (*declGenericParams)[i];
        if (!declParam || !declParam->defaultType) {
          error(typeNode.span, "Missing generic type arguments for type '" +
                                   typeNode.qualifiedName() + "'.");
          return nullptr;
        }
        activeGenericBindingsStack_.push_back(genericBindings);
        mapped = mapType(*declParam->defaultType);
        activeGenericBindingsStack_.pop_back();
        if (!mapped) {
          error(declParam->defaultType->span,
                "Unknown default generic type argument in type '" +
                    typeNode.qualifiedName() + "'.");
          return nullptr;
        }
      }
    }
    genericArgs.push_back(mapped);
    genericBindings[baseSymbol->genericParameterNames[i]] = mapped;
  }

  std::string constraintFailure;
  if (declGenericConstraints &&
      !validateGenericConstraints(*declGenericConstraints, genericBindings,
                                  &constraintFailure)) {
    error(typeNode.span, "Generic constraints not satisfied for type '" +
                             typeNode.qualifiedName() +
                             "': " + constraintFailure);
    return nullptr;
  }

  for (size_t i = 0; i < genericArgs.size(); ++i) {
    if (i != 0) {
      cacheKey += ",";
    }
    cacheKey += genericArgs[i] ? genericArgs[i]->toString() : "<?>";
  }
  cacheKey += ">";

  auto cachedIt = genericTypeInstantiations_.find(cacheKey);
  if (cachedIt != genericTypeInstantiations_.end()) {
    return cachedIt->second;
  }

  auto moduleIdIt = typeDeclarationModuleIds_.find(baseSymbol.get());
  if (moduleIdIt == typeDeclarationModuleIds_.end()) {
    error(typeNode.span,
          "Internal error: missing module information for generic type '" +
              baseSymbol->name + "'.");
    return nullptr;
  }

  auto moduleIt = modules_.find(moduleIdIt->second);
  if (moduleIt == modules_.end() || !moduleIt->second.info) {
    error(typeNode.span,
          "Internal error: missing declaration module for generic type '" +
              baseSymbol->name + "'.");
    return nullptr;
  }

  auto baseRecordType =
      std::static_pointer_cast<zir::RecordType>(baseSymbol->type);
  auto displayName =
      renderGenericTypeName(baseRecordType->getName(), genericArgs);
  auto codegenName =
      renderGenericCodegenName(baseRecordType->getCodegenName(), genericArgs);
  if (classDecl) {
    instantiatedType =
        std::make_shared<zir::ClassType>(displayName, codegenName);
  } else {
    instantiatedType =
        std::make_shared<zir::RecordType>(displayName, codegenName);
  }
  instantiatedType->setGenericInstance(
      baseRecordType->getName(), baseRecordType->getCodegenName(), genericArgs);

  auto instantiatedSymbol = std::make_shared<TypeSymbol>(
      baseSymbol->name, instantiatedType, codegenName, baseSymbol->moduleName,
      baseSymbol->visibility, baseSymbol->isUnsafe, classDecl != nullptr);
  instantiatedSymbol->isGenericInstantiation = true;
  instantiatedSymbol->genericArguments = {genericBindings.begin(),
                                          genericBindings.end()};
  genericTypeInstantiations_[cacheKey] = instantiatedSymbol;

  if (classDecl) {
    auto instantiatedClassType =
        std::static_pointer_cast<zir::ClassType>(instantiatedType);
    ClassInfo classInfo;
    classInfo.typeSymbol = instantiatedSymbol;
    classInfo.classType = instantiatedClassType;
    classInfo.ownerQualifiedName = instantiatedClassType->getName();
    classInfos_[instantiatedClassType->getName()] = classInfo;
  }

  int oldUnsafeTypeContextDepth = unsafeTypeContextDepth_;
  int oldExternTypeContextDepth = externTypeContextDepth_;
  auto oldScope = currentScope_;
  auto oldModuleId = currentModuleId_;
  if (structDecl && structDecl->isUnsafe_) {
    ++unsafeTypeContextDepth_;
  }
  if (structDecl) {
    ++externTypeContextDepth_;
  }

  currentScope_ = moduleIt->second.scope;
  currentModuleId_ = moduleIt->second.info->moduleId;

  activeGenericBindingsStack_.push_back(genericBindings);
  if (classDecl && classDecl->baseType_) {
    bool hasOwnCtor = false;
    bool hasOwnDtor = false;
    for (const auto &methodDecl : classDecl->methods_) {
      hasOwnCtor = hasOwnCtor || methodDecl->name_ == "init";
      hasOwnDtor = hasOwnDtor || methodDecl->name_ == "deinit";
    }
    auto baseType = mapType(*classDecl->baseType_);
    if (baseType && baseType->getKind() == zir::TypeKind::Class) {
      auto instantiatedClassType =
          std::static_pointer_cast<zir::ClassType>(instantiatedType);
      auto baseClass = std::static_pointer_cast<zir::ClassType>(baseType);
      instantiatedClassType->setBase(baseClass);
      auto &classInfo = classInfos_[instantiatedClassType->getName()];
      auto baseIt = classInfos_.find(baseClass->getName());
      if (baseIt != classInfos_.end()) {
        if (!hasOwnCtor) {
          classInfo.constructor = baseIt->second.constructor;
        }
        if (!hasOwnDtor) {
          classInfo.destructor = baseIt->second.destructor;
        }
        classInfo.fields = baseIt->second.fields;
        classInfo.methods.insert(baseIt->second.methods.begin(),
                                 baseIt->second.methods.end());
        classInfo.nextVirtualSlot = baseIt->second.nextVirtualSlot;
      }
      for (const auto &field : baseClass->getFields()) {
        instantiatedClassType->addField(field.name, field.type,
                                        field.visibility);
      }
    } else if (baseType) {
      error(classDecl->baseType_->span,
            "Base type of class '" + classDecl->name_ + "' must be a class.");
    }
  }

  const auto &fields =
      recordDecl ? recordDecl->fields_
                 : (structDecl ? structDecl->fields_ : classDecl->fields_);
  for (const auto &field : fields) {
    auto fieldType = mapType(*field->type);
    if (!fieldType) {
      error(field->span, "Unknown type: " + field->type->qualifiedName());
      fieldType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
    }
    instantiatedType->addField(field->name, fieldType,
                               static_cast<int>(field->visibility_));
    if (classDecl) {
      auto instantiatedClassType =
          std::static_pointer_cast<zir::ClassType>(instantiatedType);
      auto &classInfo = classInfos_[instantiatedClassType->getName()];
      classInfo.fields[field->name] = std::make_shared<VariableSymbol>(
          field->name, fieldType, false, false, field->name,
          moduleIt->second.info->moduleName, field->visibility_);
    }
  }

  activeGenericBindingsStack_.pop_back();

  unsafeTypeContextDepth_ = oldUnsafeTypeContextDepth;
  externTypeContextDepth_ = oldExternTypeContextDepth;
  currentScope_ = oldScope;
  currentModuleId_ = oldModuleId;

  if (classDecl) {
    auto instantiatedClassType =
        std::static_pointer_cast<zir::ClassType>(instantiatedType);
    auto &classInfo = classInfos_[instantiatedClassType->getName()];
    auto oldScope = currentScope_;
    auto oldModuleId = currentModuleId_;
    currentScope_ = moduleIt->second.scope;
    currentModuleId_ = moduleIt->second.info->moduleId;

    for (const auto &methodDecl : classDecl->methods_) {
      if (methodDecl->isUnsafe_) {
        ++unsafeTypeContextDepth_;
      }

      std::unordered_map<std::string, std::shared_ptr<zir::Type>>
          methodGenericBindings;
      for (const auto &genericParam : methodDecl->genericParams_) {
        if (genericParam) {
          auto placeholder = std::make_shared<zir::RecordType>(
              genericParam->typeName, genericParam->typeName);
          methodGenericBindings[genericParam->typeName] = placeholder;
        }
      }

      std::vector<std::shared_ptr<VariableSymbol>> params;
      if (!methodDecl->isStatic_) {
        params.push_back(std::make_shared<VariableSymbol>(
            "self", instantiatedClassType, false, false, "self",
            moduleIt->second.info->moduleName, Visibility::Private));
      }

      activeGenericBindingsStack_.push_back(genericBindings);
      if (!methodGenericBindings.empty()) {
        activeGenericBindingsStack_.push_back(methodGenericBindings);
      }
      for (size_t i = 0; i < methodDecl->params_.size(); ++i) {
        const auto &p = methodDecl->params_[i];
        auto mappedType = mapType(*p->type);
        if (!mappedType) {
          error(p->span, "Unknown type: " + p->type->qualifiedName());
          mappedType =
              std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
        }
        params.push_back(std::make_shared<VariableSymbol>(
            p->name, mappedType, false, p->isRef, p->name,
            moduleIt->second.info->moduleName, Visibility::Private));
      }

      std::shared_ptr<zir::Type> retType;
      bool isCtor = methodDecl->name_ == "init";
      bool isDtor = methodDecl->name_ == "deinit";
      if (isDtor && (!methodDecl->params_.empty() || methodDecl->returnType_)) {
        error(methodDecl->span,
              "Destructor 'deinit' cannot have parameters or a return type.");
      }
      if (isCtor || isDtor) {
        retType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
      } else if (methodDecl->returnType_) {
        retType = mapType(*methodDecl->returnType_);
      } else {
        retType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
      }
      if (!methodGenericBindings.empty()) {
        activeGenericBindingsStack_.pop_back();
      }
      activeGenericBindingsStack_.pop_back();

      if (!retType) {
        retType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
      }

      if (methodDecl->isUnsafe_) {
        --unsafeTypeContextDepth_;
      }

      auto methodSymbol = std::make_shared<FunctionSymbol>(
          methodDecl->name_, std::move(params), std::move(retType), "",
          moduleIt->second.info->moduleName, methodDecl->visibility_,
          methodDecl->isUnsafe_);
      for (const auto &genericParam : methodDecl->genericParams_) {
        if (genericParam) {
          methodSymbol->genericParameterNames.push_back(genericParam->typeName);
        }
      }
      methodSymbol->isMethod = !methodDecl->isStatic_;
      methodSymbol->isStatic = methodDecl->isStatic_;
      methodSymbol->isConstructor = isCtor;
      methodSymbol->isDestructor = isDtor;
      methodSymbol->ownerTypeName = instantiatedClassType->getName();
      if (methodSymbol->isMethod && !methodSymbol->isStatic &&
          !methodSymbol->isConstructor && !methodSymbol->isDestructor) {
        auto existingIt = classInfo.methods.find(methodDecl->name_);
        if (existingIt != classInfo.methods.end()) {
          auto existingMethod =
              std::dynamic_pointer_cast<FunctionSymbol>(existingIt->second);
          if (existingMethod && existingMethod->vtableSlot >= 0) {
            methodSymbol->vtableSlot = existingMethod->vtableSlot;
          }
        }
        if (methodSymbol->vtableSlot < 0) {
          methodSymbol->vtableSlot = classInfo.nextVirtualSlot++;
        }
      }
      methodSymbol->linkName = mangleName(
          moduleIt->second.info->linkPath.empty()
              ? moduleIt->second.info->moduleId
              : moduleIt->second.info->linkPath,
          instantiatedClassType->getCodegenName() + "$" + methodDecl->name_ +
              "$" + sanitizeTypeName(functionSignatureKey(*methodSymbol)));
      functionDeclarationNodes_[methodSymbol.get()] = methodDecl.get();
      functionDeclarationModuleIds_[methodSymbol.get()] =
          moduleIt->second.info->moduleId;
      functionGenericParamNames_[methodSymbol.get()] =
          methodSymbol->genericParameterNames;
      classInfo.methods[methodDecl->name_] = methodSymbol;
      if (isCtor) {
        classInfo.constructor = methodSymbol;
      } else if (isDtor) {
        classInfo.destructor = methodSymbol;
      }

      if (!methodSymbol->genericParameterNames.empty() &&
          !methodSymbol->isGenericInstantiation) {
        continue;
      }

      auto oldScope = currentScope_;
      auto oldFunction = currentFunction_;
      auto oldModuleId = currentModuleId_;
      auto oldClassStack = currentClassStack_;
      int oldUnsafeDepth = unsafeDepth_;

      currentModuleId_ = moduleIt->second.info->moduleId;
      currentScope_ = moduleIt->second.scope;
      currentFunction_ = methodSymbol;
      currentClassStack_.push_back(instantiatedClassType->getName());
      if (methodSymbol->isUnsafe) {
        ++unsafeDepth_;
      }

      pushScope();
      for (const auto &param : methodSymbol->parameters) {
        if (!currentScope_->declare(param->name, param)) {
          error(methodDecl->span,
                "Parameter '" + param->name + "' already declared.");
        }
      }

      activeGenericBindingsStack_.push_back(genericBindings);
      auto boundBody = bindBody(methodDecl->body_.get(), false);
      activeGenericBindingsStack_.pop_back();
      popScope();

      currentScope_ = oldScope;
      currentFunction_ = oldFunction;
      currentModuleId_ = oldModuleId;
      currentClassStack_ = oldClassStack;
      unsafeDepth_ = oldUnsafeDepth;

      bool hasReturn = blockAlwaysReturns(boundBody.get());

      if (!hasReturn &&
          methodSymbol->returnType->getKind() != zir::TypeKind::Void) {
        auto kind = methodSymbol->returnType->getKind();
        if (methodSymbol->returnType->isInteger() ||
            kind == zir::TypeKind::Float || kind == zir::TypeKind::Bool) {
          std::string litVal = "0";
          if (kind == zir::TypeKind::Float)
            litVal = "0.0";
          else if (kind == zir::TypeKind::Bool)
            litVal = "false";
          auto lit =
              std::make_unique<BoundLiteral>(litVal, methodSymbol->returnType);
          boundBody->statements.push_back(
              std::make_unique<BoundReturnStatement>(std::move(lit)));
        }

        _diag.report(methodDecl->span, zap::DiagnosticLevel::Warning,
                     "Function '" + methodDecl->name_ +
                         "' has non-void return type but no return on some "
                         "paths.");
      }

      boundRoot_->functions.push_back(
          std::make_unique<BoundFunctionDeclaration>(methodSymbol,
                                                     std::move(boundBody)));
    }

    currentScope_ = oldScope;
    currentModuleId_ = oldModuleId;

    auto boundRecord = std::make_unique<BoundRecordDeclaration>();
    boundRecord->type = instantiatedClassType;
    boundRoot_->records.push_back(std::move(boundRecord));
  }

  return instantiatedSymbol;
}

std::shared_ptr<zir::Type> Binder::mapTypeWithGenericBindings(
    const TypeNode &typeNode,
    const std::unordered_map<std::string, std::shared_ptr<zir::Type>>
        &genericBindings) {
  auto mapped = mapType(typeNode);
  return substituteGenericType(std::move(mapped), genericBindings);
}

std::unordered_map<std::string, std::shared_ptr<zir::Type>>
Binder::buildGenericBindings(
    const FunctionSymbol &function,
    const std::vector<std::unique_ptr<BoundExpression>> &arguments,
    const std::vector<std::unique_ptr<TypeNode>> &explicitTypeArgs,
    SourceSpan callSpan, std::string *failureReason) {
  (void)callSpan;
  std::unordered_map<std::string, std::shared_ptr<zir::Type>> bindings;

  if (function.genericParameterNames.empty()) {
    return bindings;
  }

  if (explicitTypeArgs.size() > function.genericParameterNames.size()) {
    if (failureReason) {
      *failureReason = "explicit generic argument count mismatch";
    }
    return {};
  }

  for (size_t i = 0; i < explicitTypeArgs.size(); ++i) {
    if (!explicitTypeArgs[i]) {
      continue;
    }
    auto mapped = mapType(*explicitTypeArgs[i]);
    if (!mapped) {
      if (failureReason) {
        *failureReason = "unknown generic type argument";
      }
      return {};
    }
    bindings[function.genericParameterNames[i]] = mapped;
  }

  auto declIt = functionDeclarationNodes_.find(&function);
  const auto *decl =
      declIt == functionDeclarationNodes_.end() ? nullptr : declIt->second;

  std::function<bool(const std::shared_ptr<zir::Type> &,
                     const std::shared_ptr<zir::Type> &)>
      inferFrom = [&](const std::shared_ptr<zir::Type> &paramType,
                      const std::shared_ptr<zir::Type> &argType) -> bool {
    if (!paramType || !argType) {
      return true;
    }

    if (paramType->getKind() == zir::TypeKind::Record) {
      auto rec = std::static_pointer_cast<zir::RecordType>(paramType);

      if (isVariadicViewType(paramType)) {
        const auto &fields = rec->getFields();
        if (fields.size() < 2 ||
            fields[0].type->getKind() != zir::TypeKind::Pointer) {
          return false;
        }
        auto dataPtr =
            std::static_pointer_cast<zir::PointerType>(fields[0].type);

        if (argType->getKind() == zir::TypeKind::Array) {
          auto aa = std::static_pointer_cast<zir::ArrayType>(argType);
          return inferFrom(dataPtr->getBaseType(), aa->getBaseType());
        }

        if (argType->getKind() == zir::TypeKind::Record &&
            isVariadicViewType(argType)) {
          auto argRec = std::static_pointer_cast<zir::RecordType>(argType);
          const auto &argFields = argRec->getFields();
          if (argFields.size() < 2 ||
              argFields[0].type->getKind() != zir::TypeKind::Pointer) {
            return false;
          }
          auto argDataPtr =
              std::static_pointer_cast<zir::PointerType>(argFields[0].type);
          return inferFrom(dataPtr->getBaseType(), argDataPtr->getBaseType());
        }
      }

      if (rec->isGenericInstance() &&
          argType->getKind() == zir::TypeKind::Record) {
        auto argRecord = std::static_pointer_cast<zir::RecordType>(argType);
        if (!argRecord->isGenericInstance() ||
            rec->getGenericBaseName() != argRecord->getGenericBaseName() ||
            rec->getGenericArguments().size() !=
                argRecord->getGenericArguments().size()) {
          return false;
        }
        for (size_t i = 0; i < rec->getGenericArguments().size(); ++i) {
          if (!inferFrom(rec->getGenericArguments()[i],
                         argRecord->getGenericArguments()[i])) {
            return false;
          }
        }
        return true;
      }
      auto paramName = rec->getName();
      std::string normalizedParamName =
          (!paramName.empty() && paramName[0] == '%') ? paramName.substr(1)
                                                      : paramName;
      auto isGenericName = std::find(function.genericParameterNames.begin(),
                                     function.genericParameterNames.end(),
                                     normalizedParamName) !=
                           function.genericParameterNames.end();
      if (isGenericName) {
        auto it = bindings.find(normalizedParamName);
        if (it == bindings.end()) {
          bindings[normalizedParamName] = argType;
          return true;
        }
        return it->second->toString() == argType->toString();
      }
      return true;
    }

    if (paramType->getKind() == zir::TypeKind::Class) {
      auto cls = std::static_pointer_cast<zir::ClassType>(paramType);
      if (cls->isGenericInstance() &&
          argType->getKind() == zir::TypeKind::Class) {
        auto argClass = std::static_pointer_cast<zir::ClassType>(argType);
        std::shared_ptr<zir::ClassType> matchingArgClass = argClass;
        while (matchingArgClass &&
               (!matchingArgClass->isGenericInstance() ||
                cls->getGenericBaseName() !=
                    matchingArgClass->getGenericBaseName())) {
          matchingArgClass = matchingArgClass->getBase();
        }
        if (!matchingArgClass ||
            matchingArgClass->getGenericArguments().size() !=
                cls->getGenericArguments().size()) {
          return false;
        }
        for (size_t i = 0; i < cls->getGenericArguments().size(); ++i) {
          if (!inferFrom(cls->getGenericArguments()[i],
                         matchingArgClass->getGenericArguments()[i])) {
            return false;
          }
        }
        return true;
      }
      auto paramName = cls->getName();
      std::string normalizedParamName =
          (!paramName.empty() && paramName[0] == '%') ? paramName.substr(1)
                                                      : paramName;
      auto isGenericName = std::find(function.genericParameterNames.begin(),
                                     function.genericParameterNames.end(),
                                     normalizedParamName) !=
                           function.genericParameterNames.end();
      if (isGenericName) {
        auto it = bindings.find(normalizedParamName);
        if (it == bindings.end()) {
          bindings[normalizedParamName] = argType;
          return true;
        }
        return it->second->toString() == argType->toString();
      }
      return true;
    }

    if (paramType->getKind() == zir::TypeKind::Pointer &&
        argType->getKind() == zir::TypeKind::Pointer) {
      auto pp = std::static_pointer_cast<zir::PointerType>(paramType);
      auto ap = std::static_pointer_cast<zir::PointerType>(argType);
      return inferFrom(pp->getBaseType(), ap->getBaseType());
    }

    if (paramType->getKind() == zir::TypeKind::Array &&
        argType->getKind() == zir::TypeKind::Array) {
      auto pa = std::static_pointer_cast<zir::ArrayType>(paramType);
      auto aa = std::static_pointer_cast<zir::ArrayType>(argType);
      if (pa->getSize() != aa->getSize()) {
        return false;
      }
      return inferFrom(pa->getBaseType(), aa->getBaseType());
    }

    return true;
  };

  size_t fixedCount = function.fixedParameterCount();
  for (size_t i = 0; i < arguments.size() && i < fixedCount; ++i) {
    if (!inferFrom(function.parameters[i]->type, arguments[i]->type)) {
      if (failureReason) {
        *failureReason = "conflicting generic type inference";
      }
      return {};
    }
  }

  for (const auto &name : function.genericParameterNames) {
    if (bindings.find(name) == bindings.end()) {
      bool filledFromDefault = false;
      if (decl) {
        auto paramIndex = static_cast<size_t>(std::distance(
            function.genericParameterNames.begin(),
            std::find(function.genericParameterNames.begin(),
                      function.genericParameterNames.end(), name)));
        if (paramIndex < decl->genericParams_.size() &&
            decl->genericParams_[paramIndex] &&
            decl->genericParams_[paramIndex]->defaultType) {
          activeGenericBindingsStack_.push_back(bindings);
          auto mapped = mapType(*decl->genericParams_[paramIndex]->defaultType);
          activeGenericBindingsStack_.pop_back();
          if (mapped) {
            bindings[name] = mapped;
            filledFromDefault = true;
          }
        }
      }
      if (!filledFromDefault) {
        if (failureReason) {
          *failureReason = "cannot infer generic type parameter '" + name + "'";
        }
        return {};
      }
    }
  }

  if (decl) {
    if (!validateGenericConstraints(decl->genericConstraints_, bindings,
                                    failureReason)) {
      return {};
    }
  }

  return bindings;
}

bool Binder::isSignedIntegerType(std::shared_ptr<zir::Type> type) const {
  if (!type) {
    return false;
  }

  switch (type->getKind()) {
  case zir::TypeKind::Int8:
  case zir::TypeKind::Int16:
  case zir::TypeKind::Int32:
  case zir::TypeKind::Int64:
  case zir::TypeKind::Int:
    return true;
  default:
    return false;
  }
}

bool Binder::isUnsignedIntegerType(std::shared_ptr<zir::Type> type) const {
  return type && type->isInteger() && type->isUnsigned();
}

int Binder::typeBitWidth(std::shared_ptr<zir::Type> type) const {
  if (!type) {
    return 0;
  }

  switch (type->getKind()) {
  case zir::TypeKind::Bool:
    return 1;
  case zir::TypeKind::Char:
  case zir::TypeKind::Int8:
  case zir::TypeKind::UInt8:
    return 8;
  case zir::TypeKind::Int16:
  case zir::TypeKind::UInt16:
    return 16;
  case zir::TypeKind::Int32:
  case zir::TypeKind::UInt32:
  case zir::TypeKind::Int:
  case zir::TypeKind::UInt:
  case zir::TypeKind::Float:
  case zir::TypeKind::Float32:
    return 32;
  case zir::TypeKind::Int64:
  case zir::TypeKind::UInt64:
  case zir::TypeKind::Float64:
    return 64;
  default:
    return 0;
  }
}

int Binder::conversionCost(std::shared_ptr<zir::Type> from,
                           std::shared_ptr<zir::Type> to) const {
  if (!from || !to) {
    return 1000;
  }
  if (from->toString() == to->toString()) {
    return 0;
  }
  if (isNullType(from) && isPointerType(to)) {
    return 3;
  }
  if (!canConvert(from, to)) {
    return 1000;
  }
  if (isStringType(from) && isStringType(to)) {
    return zap::text::isStringViewType(to) ? 0 : 1;
  }

  if (from->getKind() == zir::TypeKind::Enum && to->isInteger()) {
    return 1;
  }

  if (from->isFloatingPoint() && to->isFloatingPoint()) {
    return typeBitWidth(to) >= typeBitWidth(from) ? 1 : 5;
  }

  if (isSignedIntegerType(from) && isSignedIntegerType(to)) {
    return typeBitWidth(to) >= typeBitWidth(from) ? 1 : 4;
  }

  if (isUnsignedIntegerType(from) && isUnsignedIntegerType(to)) {
    return typeBitWidth(to) >= typeBitWidth(from) ? 1 : 4;
  }

  if (from->isInteger() && to->isFloatingPoint()) {
    return typeBitWidth(to) >= typeBitWidth(from) ? 2 : 5;
  }

  if (from->isFloatingPoint() && to->isInteger()) {
    return 6;
  }

  if (from->isInteger() && to->isInteger()) {
    return 5;
  }

  return 7;
}

std::string Binder::describeConversion(std::shared_ptr<zir::Type> from,
                                       std::shared_ptr<zir::Type> to) const {
  if (!from || !to) {
    return "invalid conversion";
  }
  int cost = conversionCost(from, to);
  if (cost == 0) {
    return "exact match";
  }
  if (isNullType(from) &&
      (isPointerType(to) || to->getKind() == zir::TypeKind::Class)) {
    return "null-to-reference conversion";
  }
  if (from->isFloatingPoint() && to->isFloatingPoint()) {
    return typeBitWidth(to) >= typeBitWidth(from) ? "floating widening"
                                                  : "floating narrowing";
  }
  if (isSignedIntegerType(from) && isSignedIntegerType(to)) {
    return typeBitWidth(to) >= typeBitWidth(from) ? "signed widening"
                                                  : "signed narrowing";
  }
  if (isUnsignedIntegerType(from) && isUnsignedIntegerType(to)) {
    return typeBitWidth(to) >= typeBitWidth(from) ? "unsigned widening"
                                                  : "unsigned narrowing";
  }
  if (from->isInteger() && to->isFloatingPoint()) {
    return "integer-to-float conversion";
  }
  if (from->isFloatingPoint() && to->isInteger()) {
    return "float-to-integer conversion";
  }
  if (from->isInteger() && to->isInteger()) {
    return "signedness-changing integer conversion";
  }
  return cost >= 1000 ? "not convertible" : "implicit conversion";
}

bool Binder::isSupportedBuiltInAttribute(const std::string &name) const {
  static const std::unordered_set<std::string> builtIns = {
      "error", "repr", "extern", "noMangle"};
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

    if (attr.name == "extern" || attr.name == "noMangle") {
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

    if (attr.name == "repr" || attr.name == "error") {
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
      return std::make_shared<zir::FunctionPointerType>(std::move(params),
                                                        std::move(ret));
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

bool Binder::isNumeric(std::shared_ptr<zir::Type> type) const {
  return type->isInteger() || type->isFloatingPoint();
}

bool Binder::isPointerType(std::shared_ptr<zir::Type> type) const {
  return type && type->getKind() == zir::TypeKind::Pointer;
}

bool Binder::isNullType(std::shared_ptr<zir::Type> type) const {
  return type && type->getKind() == zir::TypeKind::NullPtr;
}

bool Binder::isUnsafeActive() const {
  return unsafeDepth_ > 0 || unsafeTypeContextDepth_ > 0;
}

void Binder::requireUnsafeEnabled(SourceSpan span, const std::string &feature) {
  if (!allowUnsafe_) {
    error(span, "Using " + feature + " requires '--allow-unsafe'.");
  }
}

void Binder::requireUnsafeContext(SourceSpan span, const std::string &feature) {
  if (!isUnsafeActive()) {
    error(span, "Using " + feature + " is only allowed inside unsafe code.");
  }
}

bool Binder::canConvert(std::shared_ptr<zir::Type> from,
                        std::shared_ptr<zir::Type> to) const {
  if (!from || !to)
    return false;
  if (from->getKind() == to->getKind() && from->toString() == to->toString())
    return true;

  auto key = std::make_pair(from.get(), to.get());
  auto cacheIt = canConvertCache_.find(key);
  if (cacheIt != canConvertCache_.end())
    return cacheIt->second;

  auto &cached = canConvertCache_[key];
  if (isNullType(from) &&
      (isPointerType(to) || to->getKind() == zir::TypeKind::Class))
    return cached = true;
  if (isStringType(from) && isStringType(to))
    return cached = true;
  if (isStringType(from) && isPointerType(to)) {
    auto ptrType = std::static_pointer_cast<zir::PointerType>(to);
    return cached = (ptrType &&
                     ptrType->getBaseType()->getKind() == zir::TypeKind::Char);
  }
  if (from->getKind() == zir::TypeKind::Class &&
      to->getKind() == zir::TypeKind::Class) {
    auto fromClass = std::static_pointer_cast<zir::ClassType>(from);
    auto toClass = std::static_pointer_cast<zir::ClassType>(to);
    if (fromClass->isWeak() && !toClass->isWeak())
      return cached = false;
    for (auto current = fromClass; current; current = current->getBase()) {
      if (current->getName() == toClass->getName())
        return cached = true;
    }
  }
  if (from->getKind() == zir::TypeKind::Array && isVariadicViewType(to)) {
    auto arrayType = std::static_pointer_cast<zir::ArrayType>(from);
    auto viewType = std::static_pointer_cast<zir::RecordType>(to);
    if (!viewType->getFields().empty() &&
        viewType->getFields()[0].type->getKind() == zir::TypeKind::Pointer) {
      auto dataType = std::static_pointer_cast<zir::PointerType>(
          viewType->getFields()[0].type);
      return cached = (arrayType->getBaseType()->toString() ==
                       dataType->getBaseType()->toString());
    }
  }
  if (isFailableType(from) && isFailableType(to)) {
    auto fromValueType = failableValueType(from);
    auto fromErrorType = failableErrorType(from);
    auto toValueType = failableValueType(to);
    auto toErrorType = failableErrorType(to);
    return cached = (canConvert(fromValueType, toValueType) &&
                     canConvert(fromErrorType, toErrorType));
  }
  if (isNumeric(from) && isNumeric(to))
    return cached = true;
  if (from->getKind() == zir::TypeKind::Enum &&
      to->getKind() == zir::TypeKind::Int)
    return cached = true;
  return cached = false;
}

std::shared_ptr<zir::Type>
Binder::getPromotedType(std::shared_ptr<zir::Type> t1,
                        std::shared_ptr<zir::Type> t2) {
  if (t1->toString() == t2->toString())
    return t1;

  if (t1->isFloatingPoint() || t2->isFloatingPoint()) {
    if (t1->getKind() == zir::TypeKind::Float64 ||
        t2->getKind() == zir::TypeKind::Float64) {
      return std::make_shared<zir::PrimitiveType>(zir::TypeKind::Float64);
    }
    return std::make_shared<zir::PrimitiveType>(zir::TypeKind::Float);
  }

  // Integer promotion: preserve unsignedness when both operands are unsigned,
  // and choose width-aware integer kinds instead of always forcing Int64.
  if (isUnsignedIntegerType(t1) && isUnsignedIntegerType(t2)) {
    int width = std::max(typeBitWidth(t1), typeBitWidth(t2));
    if (width <= 8)
      return std::make_shared<zir::PrimitiveType>(zir::TypeKind::UInt8);
    if (width <= 16)
      return std::make_shared<zir::PrimitiveType>(zir::TypeKind::UInt16);
    if (width <= 32)
      return std::make_shared<zir::PrimitiveType>(zir::TypeKind::UInt);
    return std::make_shared<zir::PrimitiveType>(zir::TypeKind::UInt64);
  }

  // Mixed signed/unsigned or both signed:
  // - preserve unsignedness when the unsigned operand width is >= signed width
  //   (e.g. Int + UInt -> UInt, Int16 + UInt16 -> UInt16, Int + UInt64 ->
  //   UInt64)
  // - otherwise use signed, width-aware promotion.
  if ((isUnsignedIntegerType(t1) && isSignedIntegerType(t2)) ||
      (isSignedIntegerType(t1) && isUnsignedIntegerType(t2))) {
    auto unsignedType = isUnsignedIntegerType(t1) ? t1 : t2;
    auto signedType = isUnsignedIntegerType(t1) ? t2 : t1;

    int unsignedWidth = typeBitWidth(unsignedType);
    int signedWidth = typeBitWidth(signedType);

    if (unsignedWidth >= signedWidth) {
      if (unsignedWidth <= 8)
        return std::make_shared<zir::PrimitiveType>(zir::TypeKind::UInt8);
      if (unsignedWidth <= 16)
        return std::make_shared<zir::PrimitiveType>(zir::TypeKind::UInt16);
      if (unsignedWidth <= 32)
        return std::make_shared<zir::PrimitiveType>(zir::TypeKind::UInt);
      return std::make_shared<zir::PrimitiveType>(zir::TypeKind::UInt64);
    }

    int width = std::max(unsignedWidth, signedWidth);
    if (width <= 8)
      return std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int8);
    if (width <= 16)
      return std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int16);
    if (width <= 32)
      return std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int);
    return std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int64);
  }

  // Both signed: keep signed result, width-aware.
  int width = std::max(typeBitWidth(t1), typeBitWidth(t2));
  if (width <= 8)
    return std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int8);
  if (width <= 16)
    return std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int16);
  if (width <= 32)
    return std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int);
  return std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int64);
}

std::shared_ptr<zir::Type>
Binder::getCVariadicArgumentType(std::shared_ptr<zir::Type> type) {
  if (!type)
    return nullptr;

  if (isStringType(type)) {
    return std::make_shared<zir::PointerType>(
        std::make_shared<zir::PrimitiveType>(zir::TypeKind::Char));
  }

  if (isPointerType(type))
    return type;

  switch (type->getKind()) {
  case zir::TypeKind::Float:
  case zir::TypeKind::Float32:
    return std::make_shared<zir::PrimitiveType>(zir::TypeKind::Float64);
  case zir::TypeKind::Bool:
  case zir::TypeKind::Char:
  case zir::TypeKind::Int8:
  case zir::TypeKind::Int16:
  case zir::TypeKind::UInt8:
  case zir::TypeKind::UInt16:
    return std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int);
  case zir::TypeKind::Int32:
  case zir::TypeKind::Int64:
  case zir::TypeKind::UInt32:
  case zir::TypeKind::UInt64:
  case zir::TypeKind::Int:
  case zir::TypeKind::UInt:
  case zir::TypeKind::Float64:
    return type;
  default:
    return nullptr;
  }
}

std::unique_ptr<BoundExpression>
Binder::wrapInCast(std::unique_ptr<BoundExpression> expr,
                   std::shared_ptr<zir::Type> targetType) {
  if (!expr || !targetType)
    return expr;
  if (expr->type->getKind() == targetType->getKind() &&
      expr->type->toString() == targetType->toString()) {
    return expr;
  }
  if (isNullType(expr->type) && isPointerType(targetType)) {
    return std::make_unique<BoundCast>(std::move(expr), targetType);
  }
  return std::make_unique<BoundCast>(std::move(expr), targetType);
}

} // namespace sema
