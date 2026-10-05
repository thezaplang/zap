#include "../ast/class_decl.hpp"
#include "../ast/record_decl.hpp"
#include "binder.hpp"
#include <algorithm>

namespace sema {

void Binder::predeclareModuleTypes(ModuleState& module) {
    currentModuleId_ = module.info->moduleId;
    currentScope_ = module.scope;
    std::vector<std::pair<EnumDecl*, std::shared_ptr<TypeSymbol>>> pendingPayloadEnums;

    for (const auto& child : module.info->root->children) {
        if (auto recordDecl = dynamic_cast<RecordDecl*>(child.get())) {
            auto type = std::make_shared<zir::RecordType>(
                displayTypeName(module.info->moduleName, recordDecl->name_),
                mangleName(
                    module.info->linkPath.empty() ? module.info->moduleId : module.info->linkPath,
                    recordDecl->name_
                ),
                zir::IntrinsicTypeKind::None,
                zir::RecordRole::User,
                zir::RecordMutability::Immutable
            );
            auto symbol = std::make_shared<TypeSymbol>(
                recordDecl->name_,
                type,
                mangleName(
                    module.info->linkPath.empty() ? module.info->moduleId : module.info->linkPath,
                    recordDecl->name_
                ),
                module.info->moduleName,
                recordDecl->visibility_,
                false
            );
            validateAndApplyTypeAttributes(*recordDecl, symbol, true);
            if (symbol->hasReprC) {
                auto reprType = std::make_shared<zir::RecordType>(
                    recordDecl->name_,
                    recordDecl->name_,
                    zir::IntrinsicTypeKind::None,
                    zir::RecordRole::User,
                    zir::RecordMutability::Immutable
                );
                reprType->hasReprC = true;
                reprType->isPacked = symbol->isPacked;
                symbol->type = reprType;
            }
            for (const auto& genericParam : recordDecl->genericParams_) {
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
                error(recordDecl->span, "Type '" + recordDecl->name_ + "' already declared.");
            }
            module.symbol->members[recordDecl->name_] = symbol;
            if (recordDecl->visibility_ == Visibility::Public) {
                module.symbol->exports[recordDecl->name_] = symbol;
            }
        } else if (auto classDecl = dynamic_cast<ClassDecl*>(child.get())) {
            auto type = std::make_shared<zir::ClassType>(
                displayTypeName(module.info->moduleName, classDecl->name_),
                mangleName(
                    module.info->linkPath.empty() ? module.info->moduleId : module.info->linkPath,
                    classDecl->name_
                )
            );
            auto symbol = std::make_shared<TypeSymbol>(
                classDecl->name_,
                type,
                mangleName(
                    module.info->linkPath.empty() ? module.info->moduleId : module.info->linkPath,
                    classDecl->name_
                ),
                module.info->moduleName,
                classDecl->visibility_,
                false,
                true
            );
            for (const auto& genericParam : classDecl->genericParams_) {
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
                error(classDecl->span, "Type '" + classDecl->name_ + "' already declared.");
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
        } else if (auto interfaceDecl = dynamic_cast<InterfaceDecl*>(child.get())) {
            auto type = std::make_shared<zir::ClassType>(
                displayTypeName(module.info->moduleName, interfaceDecl->name_),
                mangleName(
                    module.info->linkPath.empty() ? module.info->moduleId : module.info->linkPath,
                    interfaceDecl->name_
                )
            );
            type->setIsInterface(true);
            auto symbol = std::make_shared<TypeSymbol>(
                interfaceDecl->name_,
                type,
                mangleName(
                    module.info->linkPath.empty() ? module.info->moduleId : module.info->linkPath,
                    interfaceDecl->name_
                ),
                module.info->moduleName,
                interfaceDecl->visibility_,
                false,
                false
            );
            symbol->isInterface = true;
            if (semanticInfo_) {
                semanticInfo_->recordDeclaration(interfaceDecl, symbol);
            }
            typeDeclarationModuleIds_[symbol.get()] = module.info->moduleId;
            if (!module.scope->declare(interfaceDecl->name_, symbol)) {
                error(interfaceDecl->span, "Type '" + interfaceDecl->name_ + "' already declared.");
            }
            module.symbol->members[interfaceDecl->name_] = symbol;
            if (interfaceDecl->visibility_ == Visibility::Public) {
                module.symbol->exports[interfaceDecl->name_] = symbol;
            }

            InterfaceInfo info;
            info.typeSymbol = symbol;
            info.classType = type;
            interfaceInfos_[type->getCodegenName()] = info;
        } else if (auto structDecl = dynamic_cast<StructDeclarationNode*>(child.get())) {
            const bool isCoreStringView =
                module.info->linkPath == "core" && structDecl->name_ == "StringView";
            const auto intrinsic = isCoreStringView ? zir::IntrinsicTypeKind::StringView
                                                    : zir::IntrinsicTypeKind::None;
            auto type = std::make_shared<zir::RecordType>(
                displayTypeName(module.info->moduleName, structDecl->name_),
                mangleName(
                    module.info->linkPath.empty() ? module.info->moduleId : module.info->linkPath,
                    structDecl->name_
                ),
                intrinsic
            );
            auto symbol = std::make_shared<TypeSymbol>(
                structDecl->name_,
                type,
                mangleName(
                    module.info->linkPath.empty() ? module.info->moduleId : module.info->linkPath,
                    structDecl->name_
                ),
                module.info->moduleName,
                structDecl->visibility_,
                structDecl->isUnsafe_
            );
            for (const auto& genericParam : structDecl->genericParams_) {
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
                auto reprType =
                    std::make_shared<zir::RecordType>(structDecl->name_, structDecl->name_);
                reprType->hasReprC = true;
                reprType->isPacked = symbol->isPacked;
                symbol->type = reprType;
            }
            if (!module.scope->declare(structDecl->name_, symbol)) {
                error(structDecl->span, "Type '" + structDecl->name_ + "' already declared.");
            }
            module.symbol->members[structDecl->name_] = symbol;
            if (structDecl->visibility_ == Visibility::Public) {
                module.symbol->exports[structDecl->name_] = symbol;
            }
        } else if (auto enumDecl = dynamic_cast<EnumDecl*>(child.get())) {
            bool hasPayloadVariants = std::any_of(
                enumDecl->entries_.begin(),
                enumDecl->entries_.end(),
                [](const EnumDecl::Entry& entry) { return entry.payloadType_ != nullptr; }
            );
            if (hasPayloadVariants) {
                auto type = std::make_shared<zir::TaggedUnionType>(
                    displayTypeName(module.info->moduleName, enumDecl->name_),
                    std::vector<zir::TaggedUnionType::Variant>{},
                    mangleName(
                        module.info->linkPath.empty() ? module.info->moduleId
                                                      : module.info->linkPath,
                        enumDecl->name_
                    )
                );
                auto symbol = std::make_shared<TypeSymbol>(
                    enumDecl->name_,
                    type,
                    mangleName(
                        module.info->linkPath.empty() ? module.info->moduleId
                                                      : module.info->linkPath,
                        enumDecl->name_
                    ),
                    module.info->moduleName,
                    enumDecl->visibility_,
                    false
                );
                validateAndApplyTypeAttributes(*enumDecl, symbol, true);
                if (semanticInfo_)
                    semanticInfo_->recordDeclaration(enumDecl, symbol);
                if (symbol->hasReprC) {
                    error(enumDecl->span, "attribute 'repr' cannot be applied to enum payloads");
                }
                if (!module.scope->declare(enumDecl->name_, symbol)) {
                    error(enumDecl->span, "Type '" + enumDecl->name_ + "' already declared.");
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

            for (const auto& entry : enumDecl->entries_) {
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
                        error(
                            enumDecl->span,
                            "Enum '" + enumDecl->name_
                                + "' has implicit value after maximum "
                                  "explicit discriminant."
                        );
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
                mangleName(
                    module.info->linkPath.empty() ? module.info->moduleId : module.info->linkPath,
                    enumDecl->name_
                )
            );
            auto symbol = std::make_shared<TypeSymbol>(
                enumDecl->name_,
                type,
                mangleName(
                    module.info->linkPath.empty() ? module.info->moduleId : module.info->linkPath,
                    enumDecl->name_
                ),
                module.info->moduleName,
                enumDecl->visibility_,
                false
            );
            validateAndApplyTypeAttributes(*enumDecl, symbol, true);
            if (semanticInfo_)
                semanticInfo_->recordDeclaration(enumDecl, symbol);
            if (symbol->hasReprC) {
                std::static_pointer_cast<zir::EnumType>(symbol->type)->hasReprC = true;
            }
            if (!module.scope->declare(enumDecl->name_, symbol)) {
                error(enumDecl->span, "Type '" + enumDecl->name_ + "' already declared.");
            }
            module.symbol->members[enumDecl->name_] = symbol;
            if (enumDecl->visibility_ == Visibility::Public) {
                module.symbol->exports[enumDecl->name_] = symbol;
            }
        }
    }

    for (auto& [decl, symbol] : pendingPayloadEnums) {
        std::vector<zir::TaggedUnionType::Variant> variants;
        variants.reserve(decl->entries_.size());
        std::unordered_set<std::string> seenVariants;
        for (size_t i = 0; i < decl->entries_.size(); ++i) {
            const auto& entry = decl->entries_[i];
            if (!seenVariants.insert(entry.name_).second) {
                error(
                    decl->span,
                    "Duplicate enum variant '" + entry.name_ + "' in '" + decl->name_ + "'."
                );
                continue;
            }
            if (entry.hasExplicitValue_) {
                error(decl->span, "Enum variants with payloads cannot use explicit values.");
            }
            std::shared_ptr<zir::Type> payloadType = nullptr;
            if (entry.payloadType_) {
                payloadType = mapType(*entry.payloadType_);
                if (!payloadType) {
                    error(
                        entry.payloadType_->span,
                        "Unknown type: " + entry.payloadType_->qualifiedName()
                    );
                    continue;
                }
            }
            variants.push_back({entry.name_, payloadType, static_cast<int64_t>(i)});
        }
        std::static_pointer_cast<zir::TaggedUnionType>(symbol->type)
            ->setVariants(std::move(variants));
    }
}

void Binder::predeclareModuleAliases(ModuleState& module) {
    currentModuleId_ = module.info->moduleId;
    currentScope_ = module.scope;

    for (const auto& child : module.info->root->children) {
        if (auto aliasDecl = dynamic_cast<TypeAliasDecl*>(child.get())) {
            auto type = mapType(*aliasDecl->type_);
            if (!type) {
                error(aliasDecl->span, "Unknown type: " + aliasDecl->type_->qualifiedName());
                continue;
            }
            auto symbol = std::make_shared<TypeSymbol>(
                aliasDecl->name_,
                type,
                mangleName(
                    module.info->linkPath.empty() ? module.info->moduleId : module.info->linkPath,
                    aliasDecl->name_
                ),
                module.info->moduleName,
                aliasDecl->visibility_,
                false
            );
            if (semanticInfo_)
                semanticInfo_->recordDeclaration(aliasDecl, symbol);
            if (!module.scope->declare(aliasDecl->name_, symbol)) {
                error(aliasDecl->span, "Type '" + aliasDecl->name_ + "' already declared.");
            }
            module.symbol->members[aliasDecl->name_] = symbol;
            if (aliasDecl->visibility_ == Visibility::Public) {
                module.symbol->exports[aliasDecl->name_] = symbol;
            }
        }
    }
}

} // namespace sema
