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

void Binder::predeclareModuleTypes(ModuleState &module) {
  currentModuleId_ = module.info->moduleId;
  currentScope_ = module.scope;

  for (const auto &child : module.info->root->children) {
    if (auto recordDecl = dynamic_cast<RecordDecl *>(child.get())) {
      auto type = std::make_shared<zir::RecordType>(
          displayTypeName(module.info->moduleName, recordDecl->name_),
          mangleName(module.info->linkPath.empty() ? module.info->moduleId
                                                   : module.info->linkPath,
                     recordDecl->name_));
      auto symbol = std::make_shared<TypeSymbol>(
          recordDecl->name_, type,
          mangleName(module.info->linkPath.empty() ? module.info->moduleId
                                                   : module.info->linkPath,
                     recordDecl->name_),
          module.info->moduleName, recordDecl->visibility_, false);
      validateAndApplyTypeAttributes(*recordDecl, symbol, true);
      if (symbol->hasReprC) {
        auto reprType = std::make_shared<zir::RecordType>(recordDecl->name_,
                                                          recordDecl->name_);
        reprType->hasReprC = true;
        symbol->type = reprType;
      }
      for (const auto &genericParam : recordDecl->genericParams_) {
        if (genericParam) {
          symbol->genericParameterNames.push_back(genericParam->typeName);
        }
      }
      recordTypeDeclarationNodes_[symbol.get()] = recordDecl;
      typeDeclarationModuleIds_[symbol.get()] = module.info->moduleId;
      if (!module.scope->declare(recordDecl->name_, symbol)) {
        error(recordDecl->span,
              "Type '" + recordDecl->name_ + "' already declared.");
      }
      module.symbol->members[recordDecl->name_] = symbol;
      if (recordDecl->visibility_ == Visibility::Public) {
        module.symbol->exports[recordDecl->name_] = symbol;
      }
    } else if (auto classDecl = dynamic_cast<ClassDecl *>(child.get())) {
      auto type = std::make_shared<zir::ClassType>(
          displayTypeName(module.info->moduleName, classDecl->name_),
          mangleName(module.info->linkPath.empty() ? module.info->moduleId
                                                   : module.info->linkPath,
                     classDecl->name_));
      auto symbol = std::make_shared<TypeSymbol>(
          classDecl->name_, type,
          mangleName(module.info->linkPath.empty() ? module.info->moduleId
                                                   : module.info->linkPath,
                     classDecl->name_),
          module.info->moduleName, classDecl->visibility_, false, true);
      for (const auto &genericParam : classDecl->genericParams_) {
        if (genericParam) {
          symbol->genericParameterNames.push_back(genericParam->typeName);
        }
      }
      classTypeDeclarationNodes_[symbol.get()] = classDecl;
      typeDeclarationModuleIds_[symbol.get()] = module.info->moduleId;
      validateAndApplyTypeAttributes(*classDecl, symbol, true);
      if (!module.scope->declare(classDecl->name_, symbol)) {
        error(classDecl->span,
              "Type '" + classDecl->name_ + "' already declared.");
      }
      module.symbol->members[classDecl->name_] = symbol;
      if (classDecl->visibility_ == Visibility::Public) {
        module.symbol->exports[classDecl->name_] = symbol;
      }

      if (symbol->genericParameterNames.empty()) {
        ClassInfo info;
        info.typeSymbol = symbol;
        info.classType = type;
        info.ownerQualifiedName = type->getName();
        classInfos_[type->getName()] = info;
      }
    } else if (auto structDecl =
                   dynamic_cast<StructDeclarationNode *>(child.get())) {
      auto type = std::make_shared<zir::RecordType>(
          displayTypeName(module.info->moduleName, structDecl->name_),
          mangleName(module.info->linkPath.empty() ? module.info->moduleId
                                                   : module.info->linkPath,
                     structDecl->name_));
      auto symbol = std::make_shared<TypeSymbol>(
          structDecl->name_, type,
          mangleName(module.info->linkPath.empty() ? module.info->moduleId
                                                   : module.info->linkPath,
                     structDecl->name_),
          module.info->moduleName, structDecl->visibility_,
          structDecl->isUnsafe_);
      for (const auto &genericParam : structDecl->genericParams_) {
        if (genericParam) {
          symbol->genericParameterNames.push_back(genericParam->typeName);
        }
      }
      structTypeDeclarationNodes_[symbol.get()] = structDecl;
      typeDeclarationModuleIds_[symbol.get()] = module.info->moduleId;
      validateAndApplyTypeAttributes(*structDecl, symbol, true);
      if (symbol->hasReprC) {
        auto reprType = std::make_shared<zir::RecordType>(structDecl->name_,
                                                          structDecl->name_);
        reprType->hasReprC = true;
        symbol->type = reprType;
      }
      if (!module.scope->declare(structDecl->name_, symbol)) {
        error(structDecl->span,
              "Type '" + structDecl->name_ + "' already declared.");
      }
      module.symbol->members[structDecl->name_] = symbol;
      if (structDecl->visibility_ == Visibility::Public) {
        module.symbol->exports[structDecl->name_] = symbol;
      }
    } else if (auto enumDecl = dynamic_cast<EnumDecl *>(child.get())) {
      std::vector<zir::EnumType::Variant> variants;
      variants.reserve(enumDecl->entries_.size());

      int64_t nextImplicitValue = 0;
      bool overflowed = false;

      for (const auto &entry : enumDecl->entries_) {
        int64_t resolvedValue = 0;

        if (entry.hasExplicitValue_) {
          resolvedValue = entry.value_;
          if (resolvedValue == std::numeric_limits<int64_t>::max()) {
            overflowed = true;
          } else {
            nextImplicitValue = resolvedValue + 1;
          }
        } else {
          if (overflowed) {
            error(enumDecl->span, "Enum '" + enumDecl->name_ +
                                      "' has implicit value after maximum "
                                      "explicit discriminant.");
            break;
          }
          resolvedValue = nextImplicitValue;
          if (nextImplicitValue == std::numeric_limits<int64_t>::max()) {
            overflowed = true;
          } else {
            ++nextImplicitValue;
          }
        }

        variants.push_back({entry.name_, resolvedValue});
      }

      auto type = std::make_shared<zir::EnumType>(
          displayTypeName(module.info->moduleName, enumDecl->name_),
          std::move(variants),
          mangleName(module.info->linkPath.empty() ? module.info->moduleId
                                                   : module.info->linkPath,
                     enumDecl->name_));
      auto symbol = std::make_shared<TypeSymbol>(
          enumDecl->name_, type,
          mangleName(module.info->linkPath.empty() ? module.info->moduleId
                                                   : module.info->linkPath,
                     enumDecl->name_),
          module.info->moduleName, enumDecl->visibility_, false);
      validateAndApplyTypeAttributes(*enumDecl, symbol, true);
      if (symbol->hasReprC) {
        std::static_pointer_cast<zir::EnumType>(symbol->type)->hasReprC = true;
      }
      if (!module.scope->declare(enumDecl->name_, symbol)) {
        error(enumDecl->span,
              "Type '" + enumDecl->name_ + "' already declared.");
      }
      module.symbol->members[enumDecl->name_] = symbol;
      if (enumDecl->visibility_ == Visibility::Public) {
        module.symbol->exports[enumDecl->name_] = symbol;
      }
    }
  }
}

void Binder::predeclareModuleAliases(ModuleState &module) {
  currentModuleId_ = module.info->moduleId;
  currentScope_ = module.scope;

  for (const auto &child : module.info->root->children) {
    if (auto aliasDecl = dynamic_cast<TypeAliasDecl *>(child.get())) {
      auto type = mapType(*aliasDecl->type_);
      if (!type) {
        error(aliasDecl->span,
              "Unknown type: " + aliasDecl->type_->qualifiedName());
        continue;
      }
      auto symbol = std::make_shared<TypeSymbol>(
          aliasDecl->name_, type,
          mangleName(module.info->linkPath.empty() ? module.info->moduleId
                                                   : module.info->linkPath,
                     aliasDecl->name_),
          module.info->moduleName, aliasDecl->visibility_, false);
      if (!module.scope->declare(aliasDecl->name_, symbol)) {
        error(aliasDecl->span,
              "Type '" + aliasDecl->name_ + "' already declared.");
      }
      module.symbol->members[aliasDecl->name_] = symbol;
      if (aliasDecl->visibility_ == Visibility::Public) {
        module.symbol->exports[aliasDecl->name_] = symbol;
      }
    }
  }
}

void Binder::applyImports(ModuleState &module, bool allowIncomplete) {
  std::map<std::string, std::string> namespaceOwners;

  for (const auto &import : module.info->imports) {
    if (!import.moduleAlias.empty() && import.targetModuleIds.size() != 1) {
      if (!allowIncomplete) {
        error(import.span, "Module alias imports are only allowed when the "
                           "path resolves to a single module.");
      }
      continue;
    }

    for (const auto &targetId : import.targetModuleIds) {
      auto targetIt = modules_.find(targetId);
      if (targetIt == modules_.end()) {
        continue;
      }

      auto &target = targetIt->second;
      auto alias = import.moduleAlias.empty() ? target.info->moduleName
                                              : import.moduleAlias;

      auto existingOwner = namespaceOwners.find(alias);
      if (existingOwner != namespaceOwners.end() &&
          existingOwner->second != targetId) {
        if (!allowIncomplete) {
          error(import.span, "Import namespace '" + alias +
                                 "' is ambiguous because multiple files share "
                                 "that module name.");
        }
        continue;
      }

      if (!module.scope->lookupLocal(alias)) {
        module.scope->declare(alias, target.symbol);
        namespaceOwners[alias] = targetId;
      } else if (module.scope->lookupLocal(alias) != target.symbol) {
        if (!allowIncomplete) {
          error(import.span, "Cannot import module '" + alias +
                                 "' because that name is already declared in "
                                 "the current file.");
        }
        continue;
      }

      if (import.bindings.empty()) {
        bool isImplicitPreludeImport =
            import.rawPath == "std/prelude" && import.moduleAlias.empty();
        if (isImplicitPreludeImport) {
          for (const auto &exported : target.symbol->exports) {
            if (!module.scope->lookupLocal(exported.first)) {
              module.scope->declare(exported.first, exported.second);
            }
          }
        }

        if (import.visibility == Visibility::Public) {
          module.symbol->exports[alias] = target.symbol;
          for (const auto &exported : target.symbol->exports) {
            module.symbol->exports[exported.first] = exported.second;
          }
        }
        continue;
      }

      if (import.targetModuleIds.size() != 1) {
        if (!allowIncomplete) {
          error(import.span, "Selective imports are only allowed when the path "
                             "resolves to a single module.");
        }
        continue;
      }

      for (const auto &binding : import.bindings) {
        auto exportedIt = target.symbol->exports.find(binding.sourceName);
        if (exportedIt == target.symbol->exports.end()) {
          if (allowIncomplete) {
            continue;
          }
          auto memberIt = target.symbol->members.find(binding.sourceName);
          if (memberIt != target.symbol->members.end() &&
              memberIt->second->visibility != Visibility::Public) {
            error(import.span, "Member '" + binding.sourceName +
                                   "' of module '" + alias + "' is private.");
          } else {
            error(import.span, "Module '" + alias + "' has no public member '" +
                                   binding.sourceName + "'.");
          }
          continue;
        }

        auto existing = module.scope->lookupLocal(binding.localName);
        if (existing && existing != exportedIt->second) {
          if (!allowIncomplete) {
            error(import.span, "Imported name '" + binding.localName +
                                   "' conflicts with an existing declaration "
                                   "in the current file.");
          }
          continue;
        }
        if (!existing) {
          module.scope->declare(binding.localName, exportedIt->second);
        }
        if (import.visibility == Visibility::Public) {
          module.symbol->exports[binding.localName] = exportedIt->second;
        }
      }
    }
  }
}

void Binder::ensureModuleValuesReady(ModuleState &module) {
  if (module.finalImportsApplied) {
    return;
  }
  if (module.valuesPreparationInProgress) {
    return;
  }

  module.valuesPreparationInProgress = true;

  for (const auto &import : module.info->imports) {
    for (const auto &targetId : import.targetModuleIds) {
      auto targetIt = modules_.find(targetId);
      if (targetIt != modules_.end()) {
        ensureModuleValuesReady(targetIt->second);
      }
    }
  }

  if (!module.valuesPredeclared) {
    predeclareModuleValues(module);
    module.valuesPredeclared = true;
  }

  if (!module.finalImportsApplied) {
    applyImports(module, false);
    module.finalImportsApplied = true;
  }

  module.valuesPreparationInProgress = false;
}

void Binder::predeclareModuleValues(ModuleState &module) {
  currentModuleId_ = module.info->moduleId;
  currentScope_ = module.scope;

  for (const auto &child : module.info->root->children) {
    if (auto funDecl = dynamic_cast<FunDecl *>(child.get())) {
      if (funDecl->isUnsafe_) {
        ++unsafeTypeContextDepth_;
      }

      std::unordered_map<std::string, std::shared_ptr<zir::Type>>
          genericBindings;
      for (const auto &genericParam : funDecl->genericParams_) {
        if (!genericParam) {
          continue;
        }
        auto placeholder = std::make_shared<zir::RecordType>(
            genericParam->typeName, genericParam->typeName);
        genericBindings[genericParam->typeName] = placeholder;
      }

      if (!genericBindings.empty()) {
        activeGenericBindingsStack_.push_back(genericBindings);
      }

      std::vector<std::shared_ptr<VariableSymbol>> params;
      for (size_t i = 0; i < funDecl->params_.size(); ++i) {
        const auto &p = funDecl->params_[i];
        if (p->isVariadic && i + 1 != funDecl->params_.size()) {
          error(p->span, "Variadic parameter must be the last parameter.");
        }
        if (p->isVariadic && p->isRef) {
          error(p->span, "Variadic parameter cannot be passed by 'ref'.");
        }
        auto mappedType = mapType(*p->type);
        if (!mappedType) {
          error(p->span, "Unknown type: " + p->type->qualifiedName());
          mappedType =
              std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
        }
        auto symbol = std::make_shared<VariableSymbol>(
            p->name, mappedType, false, p->isRef, p->name,
            module.info->moduleName, Visibility::Private);
        if (p->isVariadic) {
          symbol->is_variadic_pack = true;
          symbol->variadic_element_type = mappedType;
          symbol->type = makeVariadicViewType(mappedType);
        }
        params.push_back(std::move(symbol));
      }

      std::shared_ptr<zir::Type> retType = nullptr;
      if (funDecl->returnType_) {
        retType = mapType(*funDecl->returnType_);
      } else if (funDecl->name_ == "main" && module.info->isEntry) {
        retType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int);
      } else if (!funDecl->isExtern_ && funDecl->body_ &&
                 funDecl->genericParams_.empty()) {
        retType = nullptr;
      } else {
        retType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
      }

      if (!genericBindings.empty()) {
        activeGenericBindingsStack_.pop_back();
      }

      if (!retType && funDecl->returnType_) {
        error(funDecl->span,
              "Unknown return type in function '" + funDecl->name_ + "'.");
        retType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
      }

      if (funDecl->isUnsafe_) {
        --unsafeTypeContextDepth_;
      }

      auto linkName = (funDecl->name_ == "main" && module.info->isEntry)
                          ? std::string("main")
                          : std::string();
      auto symbol = std::make_shared<FunctionSymbol>(
          funDecl->name_, std::move(params), std::move(retType), "",
          module.info->moduleName, funDecl->visibility_, funDecl->isUnsafe_);
      symbol->returnsRef = funDecl->returnsRef_;
      for (const auto &genericParam : funDecl->genericParams_) {
        if (genericParam) {
          symbol->genericParameterNames.push_back(genericParam->typeName);
        }
      }
      functionGenericParamNames_[symbol.get()] = symbol->genericParameterNames;
      functionDeclarationNodes_[symbol.get()] = funDecl;
      functionDeclarationModuleIds_[symbol.get()] = module.info->moduleId;
      validateAndApplyFunctionAttributes(*funDecl, symbol, false);

      if (symbol->hasNoMangle ||
          (symbol->hasExternC && symbol->externAbi == "C")) {
        symbol->linkName = symbol->name;
      } else if (linkName != "main") {
        symbol->linkName = mangleFunctionName(module.info->linkPath.empty()
                                                  ? module.info->moduleId
                                                  : module.info->linkPath,
                                              *symbol);
      } else {
        symbol->linkName = linkName;
      }

      auto existing = module.scope->lookupLocal(funDecl->name_);
      if (findFunctionBySignature(existing, *symbol)) {
        error(funDecl->span,
              "Function '" + funDecl->name_ + "' already declared.");
        continue;
      }
      auto overloads = module.scope->declareFunction(funDecl->name_, symbol);
      if (!overloads) {
        error(funDecl->span, "Function '" + funDecl->name_ +
                                 "' conflicts with an existing declaration.");
        continue;
      }
      declaredFunctionSymbols_[funDecl] = symbol;
      module.symbol->members[funDecl->name_] = overloads;
      if (funDecl->visibility_ == Visibility::Public) {
        auto exportIt = module.symbol->exports.find(funDecl->name_);
        std::shared_ptr<OverloadSetSymbol> exportSet;
        if (exportIt == module.symbol->exports.end()) {
          exportSet = std::make_shared<OverloadSetSymbol>(
              funDecl->name_, module.info->moduleName, Visibility::Public);
          module.symbol->exports[funDecl->name_] = exportSet;
        } else {
          exportSet =
              std::dynamic_pointer_cast<OverloadSetSymbol>(exportIt->second);
        }
        if (!exportSet) {
          error(funDecl->span, "Function '" + funDecl->name_ +
                                   "' conflicts with an exported declaration.");
        } else {
          exportSet->addOverload(symbol);
        }
      }
    } else if (auto classDecl = dynamic_cast<ClassDecl *>(child.get())) {
      auto classSymbol = std::dynamic_pointer_cast<TypeSymbol>(
          module.scope->lookup(classDecl->name_));
      if (!classSymbol || !classSymbol->isClass ||
          !classSymbol->genericParameterNames.empty()) {
        continue;
      }
      auto classType =
          std::static_pointer_cast<zir::ClassType>(classSymbol->type);
      auto &classInfo = classInfos_[classType->getName()];

      if (classDecl->baseType_) {
        bool hasOwnCtor = false;
        bool hasOwnDtor = false;
        for (const auto &methodDecl : classDecl->methods_) {
          hasOwnCtor = hasOwnCtor || methodDecl->name_ == "init";
          hasOwnDtor = hasOwnDtor || methodDecl->name_ == "deinit";
        }
        auto baseType = mapType(*classDecl->baseType_);
        if (baseType && baseType->getKind() == zir::TypeKind::Class) {
          auto baseClass = std::static_pointer_cast<zir::ClassType>(baseType);
          auto baseIt = classInfos_.find(baseClass->getName());
          if (baseIt != classInfos_.end()) {
            if (!hasOwnCtor) {
              classInfo.constructor = baseIt->second.constructor;
            }
            if (!hasOwnDtor) {
              classInfo.destructor = baseIt->second.destructor;
            }
            classInfo.methods.insert(baseIt->second.methods.begin(),
                                     baseIt->second.methods.end());
            classInfo.nextVirtualSlot = baseIt->second.nextVirtualSlot;
          }
        }
      }

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
        if (!methodGenericBindings.empty()) {
          activeGenericBindingsStack_.push_back(methodGenericBindings);
        }

        std::vector<std::shared_ptr<VariableSymbol>> params;
        if (!methodDecl->isStatic_) {
          params.push_back(std::make_shared<VariableSymbol>(
              "self", classType, false, false, "self", module.info->moduleName,
              Visibility::Private));
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
              module.info->moduleName, Visibility::Private));
        }

        std::shared_ptr<zir::Type> retType;
        bool isCtor = methodDecl->name_ == "init";
        bool isDtor = methodDecl->name_ == "deinit";
        if (isDtor &&
            (!methodDecl->params_.empty() || methodDecl->returnType_)) {
          error(methodDecl->span,
                "Destructor 'deinit' cannot have parameters or a return type.");
        }
        if (isCtor) {
          retType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
        } else if (isDtor) {
          retType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
        } else if (methodDecl->returnType_) {
          retType = mapType(*methodDecl->returnType_);
        } else {
          retType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
        }
        if (!methodGenericBindings.empty()) {
          activeGenericBindingsStack_.pop_back();
        }
        if (!retType) {
          retType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
        }

        if (methodDecl->isUnsafe_) {
          --unsafeTypeContextDepth_;
        }

        auto symbol = std::make_shared<FunctionSymbol>(
            methodDecl->name_, std::move(params), std::move(retType), "",
            module.info->moduleName, methodDecl->visibility_,
            methodDecl->isUnsafe_);
        for (const auto &genericParam : methodDecl->genericParams_) {
          if (genericParam) {
            symbol->genericParameterNames.push_back(genericParam->typeName);
          }
        }
        symbol->isMethod = !methodDecl->isStatic_;
        symbol->isStatic = methodDecl->isStatic_;
        symbol->isConstructor = isCtor;
        symbol->isDestructor = isDtor;
        symbol->ownerTypeName = classType->getName();
        if (symbol->isMethod && !symbol->isStatic && !symbol->isConstructor &&
            !symbol->isDestructor) {
          auto existingIt = classInfo.methods.find(methodDecl->name_);
          if (existingIt != classInfo.methods.end()) {
            auto existingMethod =
                std::dynamic_pointer_cast<FunctionSymbol>(existingIt->second);
            if (existingMethod && existingMethod->vtableSlot >= 0) {
              symbol->vtableSlot = existingMethod->vtableSlot;
            }
          }
          if (symbol->vtableSlot < 0) {
            symbol->vtableSlot = classInfo.nextVirtualSlot++;
          }
        }
        symbol->linkName =
            mangleName(module.info->linkPath.empty() ? module.info->moduleId
                                                     : module.info->linkPath,
                       classDecl->name_ + "$" + methodDecl->name_ + "$" +
                           sanitizeTypeName(functionSignatureKey(*symbol)));
        declaredFunctionSymbols_[methodDecl.get()] = symbol;
        functionDeclarationNodes_[symbol.get()] = methodDecl.get();
        functionDeclarationModuleIds_[symbol.get()] = module.info->moduleId;
        functionGenericParamNames_[symbol.get()] =
            symbol->genericParameterNames;
        classInfo.methods[methodDecl->name_] = symbol;
        if (isCtor) {
          classInfo.constructor = symbol;
        } else if (isDtor) {
          classInfo.destructor = symbol;
        }
      }
    } else if (auto extDecl = dynamic_cast<ExtDecl *>(child.get())) {
      ++externTypeContextDepth_;
      std::vector<std::shared_ptr<VariableSymbol>> params;
      for (const auto &p : extDecl->params_) {
        if (p->isVariadic) {
          error(p->span, "Variadic parameters are only supported in Zap "
                         "function declarations.");
        }
        auto mappedType = mapType(*p->type);
        if (!mappedType) {
          error(p->span, "Unknown type: " + p->type->qualifiedName());
          mappedType =
              std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
        }
        params.push_back(std::make_shared<VariableSymbol>(
            p->name, mappedType, false, p->isRef, p->name,
            module.info->moduleName, Visibility::Private));
      }

      auto retType =
          extDecl->returnType_
              ? mapType(*extDecl->returnType_)
              : std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
      if (!retType) {
        error(extDecl->span, "Unknown return type in external function '" +
                                 extDecl->name_ + "'.");
        retType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
      }
      --externTypeContextDepth_;

      auto linkName = extDecl->name_;
      bool isStdFsModule =
          module.info->moduleName == "fs" &&
          module.info->sourceName.find("/std/fs.zp") != std::string::npos;
      bool isStdIoModule =
          module.info->moduleName == "io" &&
          module.info->sourceName.find("/std/io.zp") != std::string::npos;
      if (isStdFsModule && extDecl->name_ == "mkdir") {
        linkName = "zap_fs_mkdir";
      } else if (isStdFsModule && extDecl->name_ == "remove") {
        linkName = "zap_fs_remove";
      } else if (isStdFsModule && extDecl->name_ == "rename") {
        linkName = "zap_fs_rename";
      } else if (isStdIoModule && extDecl->name_ == "printf") {
        linkName = "zap_printf";
      } else if (isStdIoModule && extDecl->name_ == "printfln") {
        linkName = "zap_printfln";
      }

      auto symbol = std::make_shared<FunctionSymbol>(
          extDecl->name_, std::move(params), std::move(retType), linkName,
          module.info->moduleName, extDecl->visibility_, false,
          extDecl->isCVariadic_);
      validateAndApplyFunctionAttributes(*extDecl, symbol, true);
      if (symbol->hasNoMangle ||
          (symbol->hasExternC && symbol->externAbi == "C")) {
        symbol->linkName = symbol->name;
      }
      auto existing = module.scope->lookupLocal(extDecl->name_);
      if (findFunctionBySignature(existing, *symbol)) {
        error(extDecl->span,
              "External function '" + extDecl->name_ + "' already declared.");
        continue;
      }
      auto overloads = module.scope->declareFunction(extDecl->name_, symbol);
      if (!overloads) {
        error(extDecl->span, "External function '" + extDecl->name_ +
                                 "' conflicts with an existing declaration.");
        continue;
      }
      declaredFunctionSymbols_[extDecl] = symbol;
      module.symbol->members[extDecl->name_] = overloads;
      if (extDecl->visibility_ == Visibility::Public) {
        auto exportIt = module.symbol->exports.find(extDecl->name_);
        std::shared_ptr<OverloadSetSymbol> exportSet;
        if (exportIt == module.symbol->exports.end()) {
          exportSet = std::make_shared<OverloadSetSymbol>(
              extDecl->name_, module.info->moduleName, Visibility::Public);
          module.symbol->exports[extDecl->name_] = exportSet;
        } else {
          exportSet =
              std::dynamic_pointer_cast<OverloadSetSymbol>(exportIt->second);
        }
        if (!exportSet) {
          error(extDecl->span, "External function '" + extDecl->name_ +
                                   "' conflicts with an exported declaration.");
        } else {
          exportSet->addOverload(symbol);
        }
      }
    } else if (auto varDecl = dynamic_cast<VarDecl *>(child.get())) {
      if (!varDecl->isGlobal_) {
        continue;
      }
      std::shared_ptr<zir::Type> type;
      if (varDecl->type_) {
        type = mapType(*varDecl->type_);
        if (!type) {
          error(varDecl->span,
                "Unknown type: " + varDecl->type_->qualifiedName());
          type = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
        }
      } else {
        type = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
      }
      auto linkName = varDecl->isExternal_
                          ? varDecl->name_
                          : mangleName(module.info->linkPath.empty()
                                           ? module.info->moduleId
                                           : module.info->linkPath,
                                       varDecl->name_);
      auto symbol = std::make_shared<VariableSymbol>(
          varDecl->name_, type, false, false, linkName, module.info->moduleName,
          varDecl->visibility_);
      symbol->is_external = varDecl->isExternal_;
      if (!module.scope->declare(varDecl->name_, symbol)) {
        error(varDecl->span,
              "Variable '" + varDecl->name_ + "' already declared.");
      }
      module.symbol->members[varDecl->name_] = symbol;
      if (varDecl->visibility_ == Visibility::Public) {
        module.symbol->exports[varDecl->name_] = symbol;
      }
    } else if (auto constDecl = dynamic_cast<ConstDecl *>(child.get())) {
      std::shared_ptr<zir::Type> type;
      if (constDecl->type_) {
        type = mapType(*constDecl->type_);
        if (!type) {
          error(constDecl->span,
                "Unknown type: " + constDecl->type_->qualifiedName());
          type = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
        }
      } else {
        type = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
      }
      auto symbol = std::make_shared<VariableSymbol>(
          constDecl->name_, type, true, false,
          mangleName(module.info->linkPath.empty() ? module.info->moduleId
                                                   : module.info->linkPath,
                     constDecl->name_),
          module.info->moduleName, constDecl->visibility_);
      if (!module.scope->declare(constDecl->name_, symbol)) {
        error(constDecl->span,
              "Identifier '" + constDecl->name_ + "' already declared.");
      }
      module.symbol->members[constDecl->name_] = symbol;
      if (constDecl->visibility_ == Visibility::Public) {
        module.symbol->exports[constDecl->name_] = symbol;
      }
    }
  }
}

void Binder::visit(RootNode &node) {
  for (const auto &child : node.children) {
    if (dynamic_cast<ImportNode *>(child.get())) {
      continue;
    }
    child->accept(*this);
  }
}

void Binder::visit(ImportNode &node) { (void)node; }

void Binder::visit(ClassDecl &node) {
  auto symbol =
      std::dynamic_pointer_cast<TypeSymbol>(currentScope_->lookup(node.name_));
  if (!symbol || !symbol->isClass || !symbol->genericParameterNames.empty()) {
    return;
  }

  auto classType = std::static_pointer_cast<zir::ClassType>(symbol->type);
  auto &classInfo = classInfos_[classType->getName()];
  currentClassStack_.push_back(classType->getName());

  if (node.baseType_) {
    bool hasOwnCtor = false;
    bool hasOwnDtor = false;
    for (const auto &method : node.methods_) {
      hasOwnCtor = hasOwnCtor || method->name_ == "init";
      hasOwnDtor = hasOwnDtor || method->name_ == "deinit";
    }
    auto baseType = mapType(*node.baseType_);
    if (!baseType || baseType->getKind() != zir::TypeKind::Class) {
      error(node.baseType_->span, "Base type must be a class.");
    } else {
      auto baseClass = std::static_pointer_cast<zir::ClassType>(baseType);
      classType->setBase(baseClass);
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
        for (const auto &field : baseClass->getFields()) {
          classType->addField(field.name, field.type, field.visibility);
        }
      }
    }
  }

  for (const auto &field : node.fields_) {
    auto fieldType = mapType(*field->type);
    if (!fieldType) {
      error(field->span, "Unknown type: " + field->type->qualifiedName());
      fieldType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
    }
    classType->addField(field->name, fieldType,
                        static_cast<int>(field->visibility_));
    classInfo.fields[field->name] = std::make_shared<VariableSymbol>(
        field->name, fieldType, false, false, field->name,
        modules_[currentModuleId_].info->moduleName, field->visibility_);
  }

  auto boundRecord = std::make_unique<BoundRecordDeclaration>();
  boundRecord->type = classType;
  boundRoot_->records.push_back(std::move(boundRecord));

  for (const auto &method : node.methods_) {
    method->accept(*this);
  }

  currentClassStack_.pop_back();
}

void Binder::visit(FunDecl &node) {
  auto symbolIt = declaredFunctionSymbols_.find(&node);
  auto symbol =
      symbolIt == declaredFunctionSymbols_.end() ? nullptr : symbolIt->second;
  if (!symbol) {
    return;
  }

  if (!symbol->genericParameterNames.empty() &&
      !symbol->isGenericInstantiation) {
    return;
  }

  pushScope();
  auto oldFunction = currentFunction_;
  currentFunction_ = symbol;
  int oldUnsafeDepth = unsafeDepth_;
  if (node.isUnsafe_) {
    ++unsafeDepth_;
  }

  for (const auto &param : symbol->parameters) {
    if (!currentScope_->declare(param->name, param)) {
      error(node.span, "Parameter '" + param->name + "' already declared.");
    }
  }

  auto boundBody = bindBody(node.body_.get(), false);

  popScope();
  currentFunction_ = oldFunction;
  unsafeDepth_ = oldUnsafeDepth;

  if (!symbol->returnType) {
    std::vector<std::shared_ptr<zir::Type>> returnTypes;
    std::function<void(const BoundBlock *)> collectReturns =
        [&](const BoundBlock *block) {
          if (!block)
            return;
          for (const auto &stmt : block->statements) {
            if (auto *ret =
                    dynamic_cast<const BoundReturnStatement *>(stmt.get())) {
              returnTypes.push_back(ret->expression
                                        ? ret->expression->type
                                        : std::make_shared<zir::PrimitiveType>(
                                              zir::TypeKind::Void));
            } else if (auto *ifStmt =
                           dynamic_cast<const BoundIfStatement *>(stmt.get())) {
              collectReturns(ifStmt->thenBody.get());
              collectReturns(ifStmt->elseBody.get());
            } else if (auto *whileStmt =
                           dynamic_cast<const BoundWhileStatement *>(
                               stmt.get())) {
              collectReturns(whileStmt->body.get());
            }
          }
        };
    collectReturns(boundBody.get());

    if (returnTypes.empty()) {
      symbol->returnType =
          std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
    } else {
      auto inferred = returnTypes[0];
      bool conflict = false;
      for (size_t i = 1; i < returnTypes.size(); ++i) {
        if (!canConvert(returnTypes[i], inferred) &&
            !canConvert(inferred, returnTypes[i])) {
          error(node.span, "Cannot infer return type of function '" +
                               node.name_ + "': conflicting return types '" +
                               renderTypeForUser(inferred) + "' and '" +
                               renderTypeForUser(returnTypes[i]) +
                               "'. Add an explicit return type annotation.");
          conflict = true;
          break;
        }
        if (canConvert(inferred, returnTypes[i])) {
          inferred = returnTypes[i];
        }
      }
      symbol->returnType =
          conflict ? std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void)
                   : inferred;
    }
  }

  bool hasReturn = blockAlwaysReturns(boundBody.get());

  if (!hasReturn && symbol->linkName == "main" &&
      symbol->returnType->isInteger()) {
    auto intType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int);
    auto lit = std::make_unique<BoundLiteral>("0", intType);
    boundBody->statements.push_back(
        std::make_unique<BoundReturnStatement>(std::move(lit)));
    hasReturn = true;
  }

  if (!hasReturn && symbol->returnType->getKind() != zir::TypeKind::Void) {
    auto kind = symbol->returnType->getKind();
    if (symbol->returnType->isInteger() || kind == zir::TypeKind::Float ||
        kind == zir::TypeKind::Bool) {
      std::string litVal = "0";
      if (kind == zir::TypeKind::Float)
        litVal = "0.0";
      else if (kind == zir::TypeKind::Bool)
        litVal = "false";
      auto lit = std::make_unique<BoundLiteral>(litVal, symbol->returnType);
      boundBody->statements.push_back(
          std::make_unique<BoundReturnStatement>(std::move(lit)));
    }

    _diag.report(node.span, zap::DiagnosticLevel::Warning,
                 "Function '" + node.name_ +
                     "' has non-void return type but no return on some paths.");
  }

  boundRoot_->functions.push_back(
      std::make_unique<BoundFunctionDeclaration>(symbol, std::move(boundBody)));
}

void Binder::visit(ExtDecl &node) {
  auto symbolIt = declaredFunctionSymbols_.find(&node);
  auto symbol =
      symbolIt == declaredFunctionSymbols_.end() ? nullptr : symbolIt->second;
  if (!symbol) {
    return;
  }

  for (const auto &existing : boundRoot_->externalFunctions) {
    if (existing->symbol->linkName == symbol->linkName) {
      return;
    }
  }

  boundRoot_->externalFunctions.push_back(
      std::make_unique<BoundExternalFunctionDeclaration>(symbol));
}

void Binder::visit(VarDecl &node) {
  auto existing = currentScope_->lookupLocal(node.name_);
  std::shared_ptr<VariableSymbol> symbol;
  if (existing) {
    symbol = std::dynamic_pointer_cast<VariableSymbol>(existing);
  }

  bool isRef = node.type_ && node.type_->isReference;

  std::shared_ptr<zir::Type> type;
  std::unique_ptr<BoundExpression> initializer = nullptr;

  if (node.type_) {
    type = mapType(*node.type_);
    if (!type) {
      error(node.span, "Unknown type: " + node.type_->qualifiedName());
      type = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
    }

    if (node.initializer_) {
      initializer = bindExpressionWithExpected(node.initializer_.get(), type);
      if (initializer && !isRef) {
        if (!canConvert(initializer->type, type)) {
          error(node.initializer_->span,
                "Cannot assign expression of type '" +
                    renderTypeForUser(initializer->type) +
                    "' to variable of type '" + renderTypeForUser(type) + "'");
        } else {
          initializer = wrapInCast(std::move(initializer), type);
        }
      }
    } else if (isRef) {
      error(node.span,
            "Reference variable '" + node.name_ + "' must be initialized.");
    }
  } else {
    if (!node.initializer_) {
      error(node.span, "Variable '" + node.name_ +
                           "' needs a type annotation or an initializer.");
      type = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
    } else {
      initializer =
          bindExpressionWithExpected(node.initializer_.get(), nullptr);
      if (initializer && initializer->type &&
          initializer->type->getKind() != zir::TypeKind::Void) {
        type = initializer->type;
      } else {
        error(node.initializer_->span, "Cannot infer type of variable '" +
                                           node.name_ +
                                           "' from a void expression.");
        type = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
      }
    }
  }

  if (!symbol) {
    symbol = std::make_shared<VariableSymbol>(
        node.name_, type, false, isRef,
        node.isGlobal_ ? (node.isExternal_
                              ? node.name_
                              : mangleName(currentModuleLinkPath(), node.name_))
                       : node.name_,
        modules_[currentModuleId_].info->moduleName, node.visibility_);
    symbol->is_external = node.isExternal_;
    if (!currentScope_->declare(node.name_, symbol)) {
      error(node.span, "Variable '" + node.name_ + "' already declared.");
    }
  } else if (!node.type_ && type) {
    symbol->type = type;
  }
  symbol->is_ref = isRef;

  auto boundDecl = std::make_unique<BoundVariableDeclaration>(
      symbol, std::move(initializer));

  if (currentBlock_ && !node.isGlobal_) {
    statementStack_.push(std::move(boundDecl));
  } else {
    boundRoot_->globals.push_back(std::move(boundDecl));
  }
}

void Binder::visit(ConstDecl &node) {
  auto existing = currentScope_->lookupLocal(node.name_);
  std::shared_ptr<VariableSymbol> symbol;
  if (existing) {
    symbol = std::dynamic_pointer_cast<VariableSymbol>(existing);
  }

  std::shared_ptr<zir::Type> type;
  std::unique_ptr<BoundExpression> initializer = nullptr;

  if (node.type_) {
    type = mapType(*node.type_);
    if (!type) {
      error(node.span, "Unknown type: " + node.type_->qualifiedName());
      type = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
    }

    if (node.initializer_) {
      initializer = bindExpressionWithExpected(node.initializer_.get(), type);
      if (initializer) {
        if (!canConvert(initializer->type, type)) {
          error(node.initializer_->span,
                "Cannot assign expression of type '" +
                    renderTypeForUser(initializer->type) +
                    "' to constant of type '" + renderTypeForUser(type) + "'");
        } else {
          initializer = wrapInCast(std::move(initializer), type);
        }
      }
    } else {
      error(node.span, "Constant '" + node.name_ + "' must be initialized.");
    }
  } else {
    if (!node.initializer_) {
      error(node.span, "Constant '" + node.name_ + "' must be initialized.");
      type = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
    } else {
      initializer =
          bindExpressionWithExpected(node.initializer_.get(), nullptr);
      if (initializer && initializer->type &&
          initializer->type->getKind() != zir::TypeKind::Void) {
        type = initializer->type;
      } else {
        error(node.initializer_->span, "Cannot infer type of constant '" +
                                           node.name_ +
                                           "' from a void expression.");
        type = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
      }
    }
  }

  if (!symbol) {
    symbol = std::make_shared<VariableSymbol>(
        node.name_, type, true, false,
        mangleName(currentModuleLinkPath(), node.name_),
        modules_[currentModuleId_].info->moduleName, node.visibility_);
    if (!currentScope_->declare(node.name_, symbol)) {
      error(node.span, "Identifier '" + node.name_ + "' already declared.");
    }
  } else if (!node.type_ && type) {
    symbol->type = type;
  }

  if (initializer) {
    symbol->constant_value =
        std::shared_ptr<BoundExpression>(initializer->clone());
  }

  auto boundDecl = std::make_unique<BoundVariableDeclaration>(
      symbol, std::move(initializer));

  if (currentBlock_) {
    statementStack_.push(std::move(boundDecl));
  } else {
    boundRoot_->globals.push_back(std::move(boundDecl));
  }
}

void Binder::visit(TypeAliasDecl &node) { (void)node; }

void Binder::visit(RecordDecl &node) {
  auto symbol =
      std::dynamic_pointer_cast<TypeSymbol>(currentScope_->lookup(node.name_));
  if (!symbol || !symbol->genericParameterNames.empty()) {
    return;
  }
  auto recordType = std::static_pointer_cast<zir::RecordType>(symbol->type);

  for (const auto &field : node.fields_) {
    auto fieldType = mapType(*field->type);
    if (!fieldType) {
      error(field->span, "Unknown type: " + field->type->qualifiedName());
      fieldType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
    }
    recordType->addField(field->name, fieldType);
  }

  auto boundRecord = std::make_unique<BoundRecordDeclaration>();
  boundRecord->type = recordType;
  boundRoot_->records.push_back(std::move(boundRecord));
}

void Binder::visit(StructDeclarationNode &node) {
  auto symbol =
      std::dynamic_pointer_cast<TypeSymbol>(currentScope_->lookup(node.name_));
  if (!symbol || !symbol->genericParameterNames.empty()) {
    return;
  }
  auto recordType = std::static_pointer_cast<zir::RecordType>(symbol->type);
  int oldUnsafeTypeContextDepth = unsafeTypeContextDepth_;
  int oldExternTypeContextDepth = externTypeContextDepth_;
  if (node.isUnsafe_) {
    ++unsafeTypeContextDepth_;
  }
  ++externTypeContextDepth_;

  for (const auto &field : node.fields_) {
    auto fieldType = mapType(*field->type);
    if (!fieldType) {
      error(field->span, "Unknown type: " + field->type->qualifiedName());
      fieldType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
    }
    recordType->addField(field->name, fieldType);
  }

  unsafeTypeContextDepth_ = oldUnsafeTypeContextDepth;
  externTypeContextDepth_ = oldExternTypeContextDepth;

  auto boundRecord = std::make_unique<BoundRecordDeclaration>();
  boundRecord->type = recordType;
  boundRoot_->records.push_back(std::move(boundRecord));
}

void Binder::visit(EnumDecl &node) {
  auto symbol =
      std::dynamic_pointer_cast<TypeSymbol>(currentScope_->lookup(node.name_));
  auto enumType = std::static_pointer_cast<zir::EnumType>(symbol->type);

  auto boundEnum = std::make_unique<BoundEnumDeclaration>();
  boundEnum->type = enumType;
  boundRoot_->enums.push_back(std::move(boundEnum));
}

} // namespace sema
