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

void Binder::predeclareModuleTypes(ModuleState &module) {
  currentModuleId_ = module.info->moduleId;
  currentScope_ = module.scope;
  std::vector<std::pair<EnumDecl *, std::shared_ptr<TypeSymbol>>>
      pendingPayloadEnums;

  for (const auto &child : module.info->root->children) {
    if (auto recordDecl = dynamic_cast<RecordDecl *>(child.get())) {
      auto type = std::make_shared<zir::RecordType>(
          displayTypeName(module.info->moduleName, recordDecl->name_),
          mangleName(module.info->linkPath.empty() ? module.info->moduleId
                                                   : module.info->linkPath,
                     recordDecl->name_),
          zir::IntrinsicTypeKind::None, zir::RecordRole::User,
          zir::RecordMutability::Immutable);
      auto symbol = std::make_shared<TypeSymbol>(
          recordDecl->name_, type,
          mangleName(module.info->linkPath.empty() ? module.info->moduleId
                                                   : module.info->linkPath,
                     recordDecl->name_),
          module.info->moduleName, recordDecl->visibility_, false);
      validateAndApplyTypeAttributes(*recordDecl, symbol, true);
      if (symbol->hasReprC) {
        auto reprType = std::make_shared<zir::RecordType>(
            recordDecl->name_, recordDecl->name_, zir::IntrinsicTypeKind::None,
            zir::RecordRole::User, zir::RecordMutability::Immutable);
        reprType->hasReprC = true;
        reprType->isPacked = symbol->isPacked;
        symbol->type = reprType;
      }
      for (const auto &genericParam : recordDecl->genericParams_) {
        if (genericParam) {
          symbol->genericParameterNames.push_back(genericParam->typeName);
        }
      }
      recordTypeDeclarationNodes_[symbol.get()] = recordDecl;
      if (semanticInfo_) {
        semanticInfo_->recordDeclaration(recordDecl, symbol);
      }
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
      if (semanticInfo_) {
        semanticInfo_->recordDeclaration(classDecl, symbol);
      }
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
        classInfos_[type->getCodegenName()] = info;
      }
    } else if (auto interfaceDecl = dynamic_cast<InterfaceDecl *>(child.get())) {
      auto type = std::make_shared<zir::ClassType>(
          displayTypeName(module.info->moduleName, interfaceDecl->name_),
          mangleName(module.info->linkPath.empty() ? module.info->moduleId
                                                   : module.info->linkPath,
                     interfaceDecl->name_));
      type->setIsInterface(true);
      auto symbol = std::make_shared<TypeSymbol>(
          interfaceDecl->name_, type,
          mangleName(module.info->linkPath.empty() ? module.info->moduleId
                                                   : module.info->linkPath,
                     interfaceDecl->name_),
          module.info->moduleName, interfaceDecl->visibility_, false, false);
      symbol->isInterface = true;
      if (semanticInfo_) {
        semanticInfo_->recordDeclaration(interfaceDecl, symbol);
      }
      typeDeclarationModuleIds_[symbol.get()] = module.info->moduleId;
      if (!module.scope->declare(interfaceDecl->name_, symbol)) {
        error(interfaceDecl->span,
              "Type '" + interfaceDecl->name_ + "' already declared.");
      }
      module.symbol->members[interfaceDecl->name_] = symbol;
      if (interfaceDecl->visibility_ == Visibility::Public) {
        module.symbol->exports[interfaceDecl->name_] = symbol;
      }

      InterfaceInfo info;
      info.typeSymbol = symbol;
      info.classType = type;
      interfaceInfos_[type->getCodegenName()] = info;
    } else if (auto structDecl =
                   dynamic_cast<StructDeclarationNode *>(child.get())) {
      const bool isCoreStringView =
          module.info->linkPath == "core" && structDecl->name_ == "StringView";
      const auto intrinsic = isCoreStringView
                                 ? zir::IntrinsicTypeKind::StringView
                                 : zir::IntrinsicTypeKind::None;
      auto type = std::make_shared<zir::RecordType>(
          displayTypeName(module.info->moduleName, structDecl->name_),
          mangleName(module.info->linkPath.empty() ? module.info->moduleId
                                                   : module.info->linkPath,
                     structDecl->name_),
          intrinsic);
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
      if (semanticInfo_) {
        semanticInfo_->recordDeclaration(structDecl, symbol);
      }
      typeDeclarationModuleIds_[symbol.get()] = module.info->moduleId;
      validateAndApplyTypeAttributes(*structDecl, symbol, true);
      if (symbol->hasReprC) {
        auto reprType = std::make_shared<zir::RecordType>(structDecl->name_,
                                                          structDecl->name_);
        reprType->hasReprC = true;
        reprType->isPacked = symbol->isPacked;
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
      bool hasPayloadVariants =
          std::any_of(enumDecl->entries_.begin(), enumDecl->entries_.end(),
                      [](const EnumDecl::Entry &entry) {
                        return entry.payloadType_ != nullptr;
                      });
      if (hasPayloadVariants) {
        auto type = std::make_shared<zir::TaggedUnionType>(
            displayTypeName(module.info->moduleName, enumDecl->name_),
            std::vector<zir::TaggedUnionType::Variant>{},
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
          error(enumDecl->span,
                "attribute 'repr' cannot be applied to enum payloads");
        }
        if (!module.scope->declare(enumDecl->name_, symbol)) {
          error(enumDecl->span,
                "Type '" + enumDecl->name_ + "' already declared.");
        }
        module.symbol->members[enumDecl->name_] = symbol;
        if (enumDecl->visibility_ == Visibility::Public) {
          module.symbol->exports[enumDecl->name_] = symbol;
        }
        pendingPayloadEnums.push_back({enumDecl, symbol});
        continue;
      }

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

  for (auto &[decl, symbol] : pendingPayloadEnums) {
    std::vector<zir::TaggedUnionType::Variant> variants;
    variants.reserve(decl->entries_.size());
    std::unordered_set<std::string> seenVariants;
    for (size_t i = 0; i < decl->entries_.size(); ++i) {
      const auto &entry = decl->entries_[i];
      if (!seenVariants.insert(entry.name_).second) {
        error(decl->span, "Duplicate enum variant '" + entry.name_ + "' in '" +
                              decl->name_ + "'.");
        continue;
      }
      if (entry.hasExplicitValue_) {
        error(decl->span,
              "Enum variants with payloads cannot use explicit values.");
      }
      std::shared_ptr<zir::Type> payloadType = nullptr;
      if (entry.payloadType_) {
        payloadType = mapType(*entry.payloadType_);
        if (!payloadType) {
          error(entry.payloadType_->span,
                "Unknown type: " + entry.payloadType_->qualifiedName());
          continue;
        }
      }
      variants.push_back({entry.name_, payloadType, static_cast<int64_t>(i)});
    }
    std::static_pointer_cast<zir::TaggedUnionType>(symbol->type)
        ->setVariants(std::move(variants));
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
      if (semanticInfo_) {
        semanticInfo_->recordImportedModule(module.info->moduleId, alias,
                                            targetId);
      }

      if (import.bindings.empty()) {
        bool isImplicitStdImport =
            import.rawPath == "std/prelude" && import.moduleAlias.empty();
        if (isImplicitStdImport) {
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
        if (semanticInfo_) {
          semanticInfo_->recordImportedSymbol(module.info->moduleId,
                                              binding.localName, targetId,
                                              exportedIt->second);
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
        auto placeholder =
            zir::makeGenericParameterType(genericParam->typeName);
        genericBindings[genericParam->typeName] = placeholder;
      }

      if (!genericBindings.empty()) {
        activeGenericBindingsStack_.push_back(genericBindings);
      }

      std::vector<std::shared_ptr<VariableSymbol>> params;
      for (size_t i = 0; i < funDecl->params_.size(); ++i) {
        const auto &p = funDecl->params_[i];
        if (p->isRef && p->isSink) {
          error(p->span,
                "Parameter cannot be passed by both 'ref' and 'sink'.");
        }
        if (p->isSink && p->isNoEscape) {
          error(p->span,
                "A 'sink' parameter cannot have a 'noescape' contract.");
        }
        if (p->isVariadic && i + 1 != funDecl->params_.size()) {
          error(p->span, "Variadic parameter must be the last parameter.");
        }
        if (p->isVariadic && p->isRef) {
          error(p->span, "Variadic parameter cannot be passed by 'ref'.");
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
        auto symbol = std::make_shared<VariableSymbol>(
            p->name, mappedType, BindingKind::Mutable, p->isRef, p->name,
            module.info->moduleName, Visibility::Private);
        symbol->is_sink = p->isSink;
        symbol->is_noescape = p->isNoEscape;
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
      symbol->isEntryModule = module.info->isEntry;
      symbol->returnsRef = funDecl->returnsRef_;
      symbol->resultBorrow = resolveResultBorrowContract(
          funDecl->resultBorrowSource_, symbol->parameters, symbol->returnType,
          symbol->returnsRef, funDecl->span);
      for (const auto &genericParam : funDecl->genericParams_) {
        if (genericParam) {
          symbol->genericParameterNames.push_back(genericParam->typeName);
        }
      }
      functionGenericParamNames_[symbol.get()] = symbol->genericParameterNames;
      functionDeclarationNodes_[symbol.get()] = funDecl;
      if (semanticInfo_) {
        semanticInfo_->recordDeclaration(funDecl, symbol);
      }
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
      if (module.info->linkPath == "core" && symbol->name == "at" &&
          symbol->parameters.size() == 2 && symbol->returnType &&
          zir::isIntrinsicStringViewType(symbol->parameters[0]->type) &&
          symbol->parameters[1]->type &&
          symbol->parameters[1]->type->getKind() == zir::TypeKind::Int &&
          symbol->returnType->getKind() == zir::TypeKind::Char) {
        stringIndexFunction_ = symbol;
      }
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
      auto &classInfo = classInfos_[classType->getCodegenName()];

      std::vector<std::shared_ptr<zir::ClassType>> classInterfaces;
      auto classBase = resolveClassImplementsList(*classDecl, classInterfaces);
      if (classBase) {
        bool hasOwnCtor = false;
        bool hasOwnDtor = false;
        for (const auto &methodDecl : classDecl->methods_) {
          hasOwnCtor = hasOwnCtor || methodDecl->name_ == "init";
          hasOwnDtor = hasOwnDtor || methodDecl->name_ == "deinit";
        }
        auto baseIt = classInfos_.find(classBase->getCodegenName());
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
        for (const auto &conformance : classBase->getInterfaceConformances()) {
          classType->addInterfaceConformance(conformance);
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
            auto placeholder =
                zir::makeGenericParameterType(genericParam->typeName);
            methodGenericBindings[genericParam->typeName] = placeholder;
          }
        }
        if (!methodGenericBindings.empty()) {
          activeGenericBindingsStack_.push_back(methodGenericBindings);
        }

        std::vector<std::shared_ptr<VariableSymbol>> params;
        if (!methodDecl->isStatic_) {
          params.push_back(std::make_shared<VariableSymbol>(
              "self", classType, BindingKind::Mutable, false, "self",
              module.info->moduleName, Visibility::Private));
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
            error(p->span,
                  "'noescape' currently requires a by-value StringView "
                  "parameter.");
          }
          auto parameter = std::make_shared<VariableSymbol>(
              p->name, mappedType, BindingKind::Mutable, p->isRef, p->name,
              module.info->moduleName, Visibility::Private);
          parameter->is_sink = p->isSink;
          parameter->is_noescape = p->isNoEscape;
          params.push_back(std::move(parameter));
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
        symbol->isEntryModule = module.info->isEntry;
        for (const auto &genericParam : methodDecl->genericParams_) {
          if (genericParam) {
            symbol->genericParameterNames.push_back(genericParam->typeName);
          }
        }
        symbol->isMethod = !methodDecl->isStatic_;
        symbol->isStatic = methodDecl->isStatic_;
        symbol->returnsRef = methodDecl->returnsRef_;
        symbol->isConstructor = isCtor;
        symbol->isDestructor = isDtor;
        symbol->ownerTypeCodegenName = classType->getCodegenName();
        symbol->resultBorrow = resolveResultBorrowContract(
            methodDecl->resultBorrowSource_, symbol->parameters,
            symbol->returnType, symbol->returnsRef, methodDecl->span);
        validateAndApplyFunctionAttributes(*methodDecl, symbol, false);
        if (symbol->isMethod && !symbol->isStatic && !symbol->isConstructor &&
            !symbol->isDestructor) {
          symbol->vtableSlot = findOverriddenVtableSlot(classInfo, *symbol);
          if (symbol->vtableSlot < 0) {
            symbol->vtableSlot = classInfo.nextVirtualSlot++;
          }
        }
        if (symbol->hasNoMangle ||
            (symbol->hasExternC && symbol->externAbi == "C")) {
          symbol->linkName = symbol->name;
        } else {
          symbol->linkName =
              mangleName(module.info->linkPath.empty() ? module.info->moduleId
                                                       : module.info->linkPath,
                         classDecl->name_ + "$" + methodDecl->name_ + "$" +
                             functionSignatureKey(*symbol));
        }
        declaredFunctionSymbols_[methodDecl.get()] = symbol;
        functionDeclarationNodes_[symbol.get()] = methodDecl.get();
        if (semanticInfo_) {
          semanticInfo_->recordDeclaration(methodDecl.get(), symbol);
        }
        functionDeclarationModuleIds_[symbol.get()] = module.info->moduleId;
        functionGenericParamNames_[symbol.get()] =
            symbol->genericParameterNames;
        addClassMethodOverload(classInfo, symbol);
        if (isCtor) {
          if (!classInfo.constructor) {
            classInfo.constructor = symbol;
          }
        } else if (isDtor) {
          classInfo.destructor = symbol;
        }
      }

      bindInterfaceConformances(*classDecl, classType, classInfo,
                                classInterfaces);
    } else if (auto interfaceDecl = dynamic_cast<InterfaceDecl *>(child.get())) {
      auto interfaceSymbol = std::dynamic_pointer_cast<TypeSymbol>(
          module.scope->lookup(interfaceDecl->name_));
      if (!interfaceSymbol || !interfaceSymbol->isInterface) {
        continue;
      }
      auto interfaceType =
          std::static_pointer_cast<zir::ClassType>(interfaceSymbol->type);
      auto &interfaceInfo = interfaceInfos_[interfaceType->getCodegenName()];

      std::vector<zir::ClassType::InterfaceMethod> methodMeta;
      for (const auto &methodDecl : interfaceDecl->methods_) {
        std::vector<std::shared_ptr<VariableSymbol>> params;
        params.push_back(std::make_shared<VariableSymbol>(
            "self", interfaceType, BindingKind::Mutable, false, "self",
            module.info->moduleName, Visibility::Private));

        for (const auto &p : methodDecl->params_) {
          auto mappedType = mapType(*p->type);
          if (!mappedType) {
            error(p->span, "Unknown type: " + p->type->qualifiedName());
            mappedType =
                std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
          }
          auto parameter = std::make_shared<VariableSymbol>(
              p->name, mappedType, BindingKind::Mutable, p->isRef, p->name,
              module.info->moduleName, Visibility::Private);
          parameter->is_sink = p->isSink;
          params.push_back(std::move(parameter));
        }

        std::shared_ptr<zir::Type> retType =
            methodDecl->returnType_
                ? mapType(*methodDecl->returnType_)
                : std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
        if (!retType) {
          error(methodDecl->span, "Unknown return type in interface method '" +
                                      methodDecl->name_ + "'.");
          retType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
        }

        auto symbol = std::make_shared<FunctionSymbol>(
            methodDecl->name_, std::move(params), std::move(retType), "",
            module.info->moduleName, Visibility::Public);
        symbol->isMethod = true;
        symbol->ownerTypeCodegenName = interfaceType->getCodegenName();
        symbol->linkName = mangleName(
            module.info->linkPath.empty() ? module.info->moduleId
                                          : module.info->linkPath,
            interfaceDecl->name_ + "$" + methodDecl->name_ + "$" +
                functionSignatureKey(*symbol));

        if (interfaceInfo.methods.count(symbol->name)) {
          error(methodDecl->span, "Interface method '" + methodDecl->name_ +
                                      "' already declared.");
          continue;
        }

        interfaceInfo.methods[symbol->name] = symbol;
        boundRoot_->externalFunctions.push_back(
            std::make_unique<BoundExternalFunctionDeclaration>(symbol));
        functionDeclarationNodes_[symbol.get()] = methodDecl.get();
        functionDeclarationModuleIds_[symbol.get()] = module.info->moduleId;
        if (semanticInfo_) {
          semanticInfo_->recordDeclaration(methodDecl.get(), symbol);
        }
        methodMeta.push_back({symbol->name, symbol->linkName});
      }
      interfaceType->setInterfaceMethods(std::move(methodMeta));
      auto boundInterface = std::make_unique<BoundRecordDeclaration>();
      boundInterface->type = interfaceType;
      boundRoot_->records.push_back(std::move(boundInterface));
    } else if (auto extDecl = dynamic_cast<ExtDecl *>(child.get())) {
      ++externTypeContextDepth_;
      std::vector<std::shared_ptr<VariableSymbol>> params;
      for (const auto &p : extDecl->params_) {
        if (p->isSink) {
          error(p->span,
                "External function parameter cannot be passed by 'sink'.");
        }
        if (p->isVariadic && p->isNoEscape) {
          error(p->span,
                "Variadic parameter cannot have a 'noescape' contract.");
        }
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
        if (p->isNoEscape &&
            (p->isRef || mappedType->getIntrinsicKind() !=
                             zir::IntrinsicTypeKind::StringView)) {
          error(p->span, "'noescape' currently requires a by-value StringView "
                         "parameter.");
        }
        auto parameter = std::make_shared<VariableSymbol>(
            p->name, mappedType, BindingKind::Mutable, p->isRef, p->name,
            module.info->moduleName, Visibility::Private);
        parameter->is_sink = p->isSink;
        parameter->is_noescape = p->isNoEscape;
        params.push_back(std::move(parameter));
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
      bool isStdIoModule = module.info->linkPath == "std/io";
      if (isStdIoModule && extDecl->name_ == "printf") {
        linkName = "zap_printf";
      } else if (isStdIoModule && extDecl->name_ == "printfln") {
        linkName = "zap_printfln";
      }

      auto symbol = std::make_shared<FunctionSymbol>(
          extDecl->name_, std::move(params), std::move(retType), linkName,
          module.info->moduleName, extDecl->visibility_, false,
          extDecl->isCVariadic_);
      symbol->isExternal = true;
      symbol->isEntryModule = module.info->isEntry;
      symbol->resultBorrow = resolveResultBorrowContract(
          extDecl->resultBorrowSource_, symbol->parameters, symbol->returnType,
          false, extDecl->span);
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
    } else if (auto bindingDecl = dynamic_cast<BindingDecl *>(child.get())) {
      const bool isConstant =
          bindingDecl->kind_ == BindingKind::CompileTimeConstant;
      if (!isConstant && !bindingDecl->isGlobal_) {
        continue;
      }
      std::shared_ptr<zir::Type> type;
      if (bindingDecl->type_) {
        type = mapType(*bindingDecl->type_);
        if (!type) {
          error(bindingDecl->span,
                "Unknown type: " + bindingDecl->type_->qualifiedName());
          type = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
        }
      } else {
        type = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
      }
      auto linkName = bindingDecl->isExternal_
                          ? bindingDecl->name_
                          : mangleName(module.info->linkPath.empty()
                                           ? module.info->moduleId
                                           : module.info->linkPath,
                                       bindingDecl->name_);
      auto symbol = std::make_shared<VariableSymbol>(
          bindingDecl->name_, type, bindingDecl->kind_, false, linkName,
          module.info->moduleName, bindingDecl->visibility_);
      symbol->is_external = bindingDecl->isExternal_;
      if (!module.scope->declare(bindingDecl->name_, symbol)) {
        if (isConstant) {
          error(bindingDecl->span,
                "Identifier '" + bindingDecl->name_ + "' already declared.");
        } else {
          error(bindingDecl->span,
                "Variable '" + bindingDecl->name_ + "' already declared.");
        }
      }
      module.symbol->members[bindingDecl->name_] = symbol;
      if (bindingDecl->visibility_ == Visibility::Public) {
        module.symbol->exports[bindingDecl->name_] = symbol;
      }
    }
  }
}
} // namespace sema
