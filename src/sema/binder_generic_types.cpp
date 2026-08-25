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
    cacheKey +=
        genericArgs[i] ? typeInterner_.mangleKey(genericArgs[i]) : "missing";
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
  instantiatedType->hasReprC = baseRecordType->hasReprC;
  instantiatedType->isPacked = baseRecordType->isPacked;
  instantiatedType->setMutability(baseRecordType->getMutability());

  auto instantiatedSymbol = std::make_shared<TypeSymbol>(
      baseSymbol->name, instantiatedType, codegenName, baseSymbol->moduleName,
      baseSymbol->visibility, baseSymbol->isUnsafe, classDecl != nullptr);
  instantiatedSymbol->isGenericInstantiation = true;
  instantiatedSymbol->hasReprC = baseSymbol->hasReprC;
  instantiatedSymbol->isPacked = baseSymbol->isPacked;
  instantiatedSymbol->genericArguments = {genericBindings.begin(),
                                          genericBindings.end()};
  genericTypeInstantiations_[cacheKey] = instantiatedSymbol;
  boundRoot_->genericTypes.push_back(instantiatedType);

  if (classDecl) {
    auto instantiatedClassType =
        std::static_pointer_cast<zir::ClassType>(instantiatedType);
    ClassInfo classInfo;
    classInfo.typeSymbol = instantiatedSymbol;
    classInfo.classType = instantiatedClassType;
    classInfo.ownerQualifiedName = instantiatedClassType->getName();
    classInfos_[instantiatedClassType->getCodegenName()] = classInfo;
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
  if (classDecl && !classDecl->implementsList_.empty()) {
    bool hasOwnCtor = false;
    bool hasOwnDtor = false;
    for (const auto &methodDecl : classDecl->methods_) {
      hasOwnCtor = hasOwnCtor || methodDecl->name_ == "init";
      hasOwnDtor = hasOwnDtor || methodDecl->name_ == "deinit";
    }
    std::vector<std::shared_ptr<zir::ClassType>> interfaces;
    auto baseClass = resolveClassImplementsList(*classDecl, interfaces);
    if (baseClass) {
      auto instantiatedClassType =
          std::static_pointer_cast<zir::ClassType>(instantiatedType);
      instantiatedClassType->setBase(baseClass);
      auto &classInfo = classInfos_[instantiatedClassType->getCodegenName()];
      auto baseIt = classInfos_.find(baseClass->getCodegenName());
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
      auto &classInfo = classInfos_[instantiatedClassType->getCodegenName()];
      classInfo.fields[field->name] = std::make_shared<VariableSymbol>(
          field->name, fieldType, BindingKind::Mutable, false, field->name,
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
    auto &classInfo = classInfos_[instantiatedClassType->getCodegenName()];
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
          auto placeholder =
              zir::makeGenericParameterType(genericParam->typeName);
          methodGenericBindings[genericParam->typeName] = placeholder;
        }
      }

      std::vector<std::shared_ptr<VariableSymbol>> params;
      if (!methodDecl->isStatic_) {
        params.push_back(std::make_shared<VariableSymbol>(
            "self", instantiatedClassType, BindingKind::Mutable, false, "self",
            moduleIt->second.info->moduleName, Visibility::Private));
      }

      activeGenericBindingsStack_.push_back(genericBindings);
      if (!methodGenericBindings.empty()) {
        activeGenericBindingsStack_.push_back(methodGenericBindings);
      }
      for (size_t i = 0; i < methodDecl->params_.size(); ++i) {
        const auto &p = methodDecl->params_[i];
        if (p->isRef && p->isSink) {
          error(p->span,
                "Parameter cannot be passed by both 'ref' and 'sink'.");
        }
        if (p->isSink && p->isNoEscape) {
          error(p->span,
                "A 'sink' parameter cannot have a 'noescape' contract.");
        }
        if (p->isVariadic && p->isSink) {
          error(p->span, "Variadic parameter cannot be passed by 'sink'.");
        }
        if (p->isVariadic && p->isNoEscape) {
          error(p->span,
                "Variadic parameter cannot have a 'noescape' contract.");
        }
        auto mappedType = mapType(*p->type);
        if (!mappedType) {
          error(p->span, "Unknown type: " + p->type->qualifiedName());
          mappedType =
              std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
        }
        if (p->isNoEscape &&
            (p->isRef || mappedType->getIntrinsicKind() !=
                             zir::IntrinsicTypeKind::StringView)) {
          error(p->span, "'noescape' currently requires a by-value StringView "
                         "parameter.");
        }
        auto parameter = std::make_shared<VariableSymbol>(
            p->name, mappedType, BindingKind::Mutable, p->isRef, p->name,
            moduleIt->second.info->moduleName, Visibility::Private);
        parameter->is_sink = p->isSink;
        parameter->is_noescape = p->isNoEscape;
        params.push_back(std::move(parameter));
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
      methodSymbol->isEntryModule = moduleIt->second.info->isEntry;
      for (const auto &genericParam : methodDecl->genericParams_) {
        if (genericParam) {
          methodSymbol->genericParameterNames.push_back(genericParam->typeName);
        }
      }
      methodSymbol->isMethod = !methodDecl->isStatic_;
      methodSymbol->isStatic = methodDecl->isStatic_;
      methodSymbol->returnsRef = methodDecl->returnsRef_;
      methodSymbol->isConstructor = isCtor;
      methodSymbol->isDestructor = isDtor;
      methodSymbol->ownerTypeCodegenName =
          instantiatedClassType->getCodegenName();
      methodSymbol->resultBorrow = resolveResultBorrowContract(
          methodDecl->resultBorrowSource_, methodSymbol->parameters,
          methodSymbol->returnType, methodSymbol->returnsRef, methodDecl->span);
      validateAndApplyFunctionAttributes(*methodDecl, methodSymbol, false);
      if (methodSymbol->isMethod && !methodSymbol->isStatic &&
          !methodSymbol->isConstructor && !methodSymbol->isDestructor) {
        methodSymbol->vtableSlot =
            findOverriddenVtableSlot(classInfo, *methodSymbol);
        if (methodSymbol->vtableSlot < 0) {
          methodSymbol->vtableSlot = classInfo.nextVirtualSlot++;
        }
      }
      if (methodSymbol->hasNoMangle ||
          (methodSymbol->hasExternC && methodSymbol->externAbi == "C")) {
        methodSymbol->linkName = methodSymbol->name;
      } else {
        methodSymbol->linkName = mangleName(
            moduleIt->second.info->linkPath.empty()
                ? moduleIt->second.info->moduleId
                : moduleIt->second.info->linkPath,
            instantiatedClassType->getCodegenName() + "$" + methodDecl->name_ +
                "$" + functionSignatureKey(*methodSymbol));
      }
      functionDeclarationNodes_[methodSymbol.get()] = methodDecl.get();
      functionDeclarationModuleIds_[methodSymbol.get()] =
          moduleIt->second.info->moduleId;
      functionGenericParamNames_[methodSymbol.get()] =
          methodSymbol->genericParameterNames;
      addClassMethodOverload(classInfo, methodSymbol);
      if (isCtor) {
        if (!classInfo.constructor) {
          classInfo.constructor = methodSymbol;
        }
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
            methodSymbol->returnType->isFloatingPoint() ||
            kind == zir::TypeKind::Bool) {
          std::string litVal = "0";
          if (methodSymbol->returnType->isFloatingPoint())
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
      if (rec->getRole() == zir::RecordRole::GenericParameter) {
        const auto &paramName = rec->getName();
        auto it = bindings.find(paramName);
        if (it == bindings.end()) {
          bindings[paramName] = argType;
          return true;
        }
        return typeInterner_.same(it->second, argType);
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
      const auto &paramName = cls->getName();
      auto isGenericName =
          std::find(function.genericParameterNames.begin(),
                    function.genericParameterNames.end(),
                    paramName) != function.genericParameterNames.end();
      if (isGenericName) {
        auto it = bindings.find(paramName);
        if (it == bindings.end()) {
          bindings[paramName] = argType;
          return true;
        }
        return typeInterner_.same(it->second, argType);
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
} // namespace sema
