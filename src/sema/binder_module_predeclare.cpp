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

namespace {

// `ref self` changes how a receiver is passed, but is not an overload
// discriminator at a member call site.  Keep extension declarations that
// differ only in receiver mode from creating an ambiguous call.
bool sameExtensionCallSignature(const FunctionSymbol& lhs, const FunctionSymbol& rhs) {
    if (lhs.parameters.empty() || rhs.parameters.empty()
        || lhs.parameters.size() != rhs.parameters.size() || lhs.isCVariadic != rhs.isCVariadic) {
        return false;
    }

    for (size_t i = 1; i < lhs.parameters.size(); ++i) {
        const auto& left = lhs.parameters[i];
        const auto& right = rhs.parameters[i];
        if (left->is_ref != right->is_ref || left->is_variadic_pack != right->is_variadic_pack
            || !left->type || !right->type || !zir::sameType(left->type, right->type)) {
            return false;
        }
    }
    return true;
}

bool sameExtensionTypePattern(
    const std::shared_ptr<zir::Type>& lhs,
    const std::shared_ptr<zir::Type>& rhs,
    std::unordered_map<std::string, std::string>& left_to_right,
    std::unordered_map<std::string, std::string>& right_to_left
) {
    if (!lhs || !rhs) {
        return false;
    }
    if (lhs->getKind() == zir::TypeKind::Record) {
        const auto left_record = std::static_pointer_cast<zir::RecordType>(lhs);
        if (left_record->getRole() == zir::RecordRole::GenericParameter) {
            if (rhs->getKind() != zir::TypeKind::Record) {
                return false;
            }
            const auto right_record = std::static_pointer_cast<zir::RecordType>(rhs);
            if (right_record->getRole() != zir::RecordRole::GenericParameter) {
                return false;
            }
            const auto left_it = left_to_right.find(left_record->getName());
            const auto right_it = right_to_left.find(right_record->getName());
            if ((left_it != left_to_right.end() && left_it->second != right_record->getName())
                || (right_it != right_to_left.end()
                    && right_it->second != left_record->getName())) {
                return false;
            }
            left_to_right[left_record->getName()] = right_record->getName();
            right_to_left[right_record->getName()] = left_record->getName();
            return true;
        }
        if (left_record->isGenericInstance() && rhs->getKind() == zir::TypeKind::Record) {
            const auto right_record = std::static_pointer_cast<zir::RecordType>(rhs);
            if (!right_record->isGenericInstance()
                || left_record->getGenericBaseName() != right_record->getGenericBaseName()
                || left_record->getGenericArguments().size()
                    != right_record->getGenericArguments().size()) {
                return false;
            }
            for (size_t i = 0; i < left_record->getGenericArguments().size(); ++i) {
                if (!sameExtensionTypePattern(
                        left_record->getGenericArguments()[i],
                        right_record->getGenericArguments()[i],
                        left_to_right,
                        right_to_left
                    )) {
                    return false;
                }
            }
            return true;
        }
        if (isVariadicViewType(lhs) && isVariadicViewType(rhs)) {
            const auto right_record = std::static_pointer_cast<zir::RecordType>(rhs);
            const auto& left_fields = left_record->getFields();
            const auto& right_fields = right_record->getFields();
            if (left_fields.empty() || right_fields.empty()
                || left_fields.front().type->getKind() != zir::TypeKind::Pointer
                || right_fields.front().type->getKind() != zir::TypeKind::Pointer) {
                return false;
            }
            return sameExtensionTypePattern(
                std::static_pointer_cast<zir::PointerType>(left_fields.front().type)->getBaseType(),
                std::static_pointer_cast<zir::PointerType>(right_fields.front().type)
                    ->getBaseType(),
                left_to_right,
                right_to_left
            );
        }
    }
    if (lhs->getKind() == zir::TypeKind::Class && rhs->getKind() == zir::TypeKind::Class) {
        const auto left_class = std::static_pointer_cast<zir::ClassType>(lhs);
        const auto right_class = std::static_pointer_cast<zir::ClassType>(rhs);
        if (left_class->isGenericInstance() && right_class->isGenericInstance()) {
            if (left_class->getGenericBaseName() != right_class->getGenericBaseName()
                || left_class->getGenericArguments().size()
                    != right_class->getGenericArguments().size()) {
                return false;
            }
            for (size_t i = 0; i < left_class->getGenericArguments().size(); ++i) {
                if (!sameExtensionTypePattern(
                        left_class->getGenericArguments()[i],
                        right_class->getGenericArguments()[i],
                        left_to_right,
                        right_to_left
                    )) {
                    return false;
                }
            }
            return true;
        }
    }
    if (lhs->getKind() == zir::TypeKind::Pointer && rhs->getKind() == zir::TypeKind::Pointer) {
        return sameExtensionTypePattern(
            std::static_pointer_cast<zir::PointerType>(lhs)->getBaseType(),
            std::static_pointer_cast<zir::PointerType>(rhs)->getBaseType(),
            left_to_right,
            right_to_left
        );
    }
    if (lhs->getKind() == zir::TypeKind::Array && rhs->getKind() == zir::TypeKind::Array) {
        const auto left_array = std::static_pointer_cast<zir::ArrayType>(lhs);
        const auto right_array = std::static_pointer_cast<zir::ArrayType>(rhs);
        return left_array->getSize() == right_array->getSize()
            && sameExtensionTypePattern(
                left_array->getBaseType(),
                right_array->getBaseType(),
                left_to_right,
                right_to_left
            );
    }
    return zir::sameType(lhs, rhs);
}

bool sameGenericExtensionSignature(const FunctionSymbol& lhs, const FunctionSymbol& rhs) {
    if (lhs.parameters.size() != rhs.parameters.size() || lhs.isCVariadic != rhs.isCVariadic) {
        return false;
    }
    std::unordered_map<std::string, std::string> left_to_right;
    std::unordered_map<std::string, std::string> right_to_left;
    for (size_t i = 0; i < lhs.parameters.size(); ++i) {
        const auto& left = lhs.parameters[i];
        const auto& right = rhs.parameters[i];
        if (left->is_ref != right->is_ref || left->is_variadic_pack != right->is_variadic_pack
            || !sameExtensionTypePattern(left->type, right->type, left_to_right, right_to_left)) {
            return false;
        }
    }
    return true;
}

bool containsGenericParameter(const std::shared_ptr<zir::Type>& type, std::string_view name) {
    if (!type) {
        return false;
    }
    if (type->getKind() == zir::TypeKind::Record) {
        const auto record = std::static_pointer_cast<zir::RecordType>(type);
        if (record->getRole() == zir::RecordRole::GenericParameter) {
            return record->getName() == name;
        }
        if (record->isGenericInstance()) {
            for (const auto& argument : record->getGenericArguments()) {
                if (containsGenericParameter(argument, name)) {
                    return true;
                }
            }
        }
        if (isVariadicViewType(type)) {
            const auto& fields = record->getFields();
            if (!fields.empty() && fields.front().type
                && fields.front().type->getKind() == zir::TypeKind::Pointer) {
                return containsGenericParameter(
                    std::static_pointer_cast<zir::PointerType>(fields.front().type)->getBaseType(),
                    name
                );
            }
        }
        return false;
    }
    if (type->getKind() == zir::TypeKind::Class) {
        const auto classType = std::static_pointer_cast<zir::ClassType>(type);
        for (const auto& argument : classType->getGenericArguments()) {
            if (containsGenericParameter(argument, name)) {
                return true;
            }
        }
        return false;
    }
    if (type->getKind() == zir::TypeKind::Pointer) {
        return containsGenericParameter(
            std::static_pointer_cast<zir::PointerType>(type)->getBaseType(),
            name
        );
    }
    if (type->getKind() == zir::TypeKind::Array) {
        return containsGenericParameter(
            std::static_pointer_cast<zir::ArrayType>(type)->getBaseType(),
            name
        );
    }
    return false;
}

} // namespace

void Binder::applyImports(ModuleState& module, bool allowIncomplete) {
    std::map<std::string, std::string> namespaceOwners;

    for (const auto& import : module.info->imports) {
        if (!import.moduleAlias.empty() && import.targetModuleIds.size() != 1) {
            if (!allowIncomplete) {
                error(
                    import.span,
                    "Module alias imports are only allowed when the "
                    "path resolves to a single module."
                );
            }
            continue;
        }

        for (const auto& targetId : import.targetModuleIds) {
            auto targetIt = modules_.find(targetId);
            if (targetIt == modules_.end()) {
                continue;
            }

            auto& target = targetIt->second;
            auto alias = import.moduleAlias.empty() ? target.info->moduleName : import.moduleAlias;

            auto existingOwner = namespaceOwners.find(alias);
            if (existingOwner != namespaceOwners.end() && existingOwner->second != targetId) {
                if (!allowIncomplete) {
                    error(
                        import.span,
                        "Import namespace '" + alias
                            + "' is ambiguous because multiple files share "
                              "that module name."
                    );
                }
                continue;
            }

            if (!module.scope->lookupLocal(alias)) {
                module.scope->declare(alias, target.symbol);
                namespaceOwners[alias] = targetId;
            } else if (module.scope->lookupLocal(alias) != target.symbol) {
                if (!allowIncomplete) {
                    error(
                        import.span,
                        "Cannot import module '" + alias
                            + "' because that name is already declared in "
                              "the current file."
                    );
                }
                continue;
            }
            if (semanticInfo_) {
                semanticInfo_->recordImportedModule(module.info->moduleId, alias, targetId);
            }

            if (import.bindings.empty()) {
                bool isImplicitStdImport =
                    import.rawPath == "std/prelude" && import.moduleAlias.empty();
                if (isImplicitStdImport) {
                    for (const auto& exported : target.symbol->exports) {
                        if (!module.scope->lookupLocal(exported.first)) {
                            module.scope->declare(exported.first, exported.second);
                        }
                    }
                }

                if (import.visibility == Visibility::Public) {
                    module.symbol->exports[alias] = target.symbol;
                    for (const auto& exported : target.symbol->exports) {
                        module.symbol->exports[exported.first] = exported.second;
                    }
                }
                continue;
            }

            if (import.targetModuleIds.size() != 1) {
                if (!allowIncomplete) {
                    error(
                        import.span,
                        "Selective imports are only allowed when the path "
                        "resolves to a single module."
                    );
                }
                continue;
            }

            for (const auto& binding : import.bindings) {
                auto exportedIt = target.symbol->exports.find(binding.sourceName);
                if (exportedIt == target.symbol->exports.end()) {
                    if (binding.importsMacro) {
                        continue;
                    }
                    if (allowIncomplete) {
                        continue;
                    }
                    auto memberIt = target.symbol->members.find(binding.sourceName);
                    if (memberIt != target.symbol->members.end()
                        && memberIt->second->visibility != Visibility::Public) {
                        error(
                            import.span,
                            "Member '" + binding.sourceName + "' of module '" + alias
                                + "' is private."
                        );
                    } else {
                        error(
                            import.span,
                            "Module '" + alias + "' has no public member '" + binding.sourceName
                                + "'."
                        );
                    }
                    continue;
                }

                auto existing = module.scope->lookupLocal(binding.localName);
                if (existing && existing != exportedIt->second) {
                    if (!allowIncomplete) {
                        error(
                            import.span,
                            "Imported name '" + binding.localName
                                + "' conflicts with an existing declaration "
                                  "in the current file."
                        );
                    }
                    continue;
                }
                if (!existing) {
                    module.scope->declare(binding.localName, exportedIt->second);
                }
                if (semanticInfo_) {
                    semanticInfo_->recordImportedSymbol(
                        module.info->moduleId,
                        binding.localName,
                        targetId,
                        exportedIt->second
                    );
                }
                if (import.visibility == Visibility::Public) {
                    module.symbol->exports[binding.localName] = exportedIt->second;
                }
            }
        }
    }
}

void Binder::ensureModuleValuesReady(ModuleState& module) {
    if (module.finalImportsApplied) {
        return;
    }
    if (module.valuesPreparationInProgress) {
        return;
    }

    module.valuesPreparationInProgress = true;

    for (const auto& import : module.info->imports) {
        for (const auto& targetId : import.targetModuleIds) {
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

void Binder::predeclareModuleValues(ModuleState& module) {
    currentModuleId_ = module.info->moduleId;
    currentScope_ = module.scope;

    for (const auto& child : module.info->root->children) {
        if (auto funDecl = dynamic_cast<FunDecl*>(child.get())) {
            if (predeclareCtfeFunction(module, *funDecl))
                continue;
            if (funDecl->isUnsafe_) {
                ++unsafeTypeContextDepth_;
            }

            std::unordered_map<std::string, std::shared_ptr<zir::Type>> genericBindings;
            for (const auto& genericParam : funDecl->genericParams_) {
                if (!genericParam) {
                    continue;
                }
                auto placeholder = zir::makeGenericParameterType(genericParam->typeName);
                genericBindings[genericParam->typeName] = placeholder;
            }

            if (!genericBindings.empty()) {
                activeGenericBindingsStack_.push_back(genericBindings);
            }

            std::vector<std::shared_ptr<VariableSymbol>> params;
            for (size_t i = 0; i < funDecl->params_.size(); ++i) {
                const auto& p = funDecl->params_[i];
                if (p->isRef && p->isSink) {
                    error(p->span, "Parameter cannot be passed by both 'ref' and 'sink'.");
                }
                if (p->isSink && p->isNoEscape) {
                    error(p->span, "A 'sink' parameter cannot have a 'noescape' contract.");
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
                    error(p->span, "Variadic parameter cannot have a 'noescape' contract.");
                }
                auto mappedType = mapType(*p->type);
                if (!mappedType) {
                    error(p->span, "Unknown type: " + p->type->qualifiedName());
                    mappedType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
                }
                if (p->isNoEscape
                    && (p->isRef
                        || mappedType->getIntrinsicKind() != zir::IntrinsicTypeKind::StringView)) {
                    error(
                        p->span,
                        "'noescape' currently requires a by-value StringView "
                        "parameter."
                    );
                }
                auto symbol = std::make_shared<VariableSymbol>(
                    p->name,
                    mappedType,
                    BindingKind::Mutable,
                    p->isRef,
                    p->name,
                    module.info->moduleName,
                    Visibility::Private
                );
                symbol->syntaxName = p->syntaxName;
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
            } else if (!funDecl->isExtern_ && funDecl->body_ && funDecl->genericParams_.empty()) {
                retType = nullptr;
            } else {
                retType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
            }

            if (!genericBindings.empty()) {
                activeGenericBindingsStack_.pop_back();
            }

            if (!retType && funDecl->returnType_) {
                error(funDecl->span, "Unknown return type in function '" + funDecl->name_ + "'.");
                retType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
            }

            if (funDecl->isUnsafe_) {
                --unsafeTypeContextDepth_;
            }

            auto linkName = (funDecl->name_ == "main" && module.info->isEntry) ? std::string("main")
                                                                               : std::string();
            auto symbol = std::make_shared<FunctionSymbol>(
                funDecl->name_,
                std::move(params),
                std::move(retType),
                "",
                module.info->moduleName,
                funDecl->visibility_,
                funDecl->isUnsafe_
            );
            symbol->isEntryModule = module.info->isEntry;
            symbol->returnsRef = funDecl->returnsRef_;
            symbol->resultBorrow = resolveResultBorrowContract(
                funDecl->resultBorrowSource_,
                symbol->parameters,
                symbol->returnType,
                symbol->returnsRef,
                funDecl->span,
                &funDecl->resultBorrowName_
            );
            for (const auto& genericParam : funDecl->genericParams_) {
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

            if (symbol->hasNoMangle || (symbol->hasExternC && symbol->externAbi == "C")) {
                symbol->linkName = symbol->name;
            } else if (linkName != "main") {
                symbol->linkName = mangleFunctionName(
                    module.info->linkPath.empty() ? module.info->moduleId : module.info->linkPath,
                    *symbol
                );
            } else {
                symbol->linkName = linkName;
            }

            auto existing = module.scope->lookupLocal(funDecl->name_);
            if (findFunctionBySignature(existing, *symbol)) {
                error(funDecl->span, "Function '" + funDecl->name_ + "' already declared.");
                continue;
            }
            auto overloads = module.scope->declareFunction(funDecl->name_, symbol);
            if (!overloads) {
                error(
                    funDecl->span,
                    "Function '" + funDecl->name_ + "' conflicts with an existing declaration."
                );
                continue;
            }
            declaredFunctionSymbols_[funDecl] = symbol;
            if (module.info->linkPath == "core" && symbol->name == "at"
                && symbol->parameters.size() == 2 && symbol->returnType
                && zir::isIntrinsicStringViewType(symbol->parameters[0]->type)
                && symbol->parameters[1]->type
                && symbol->parameters[1]->type->getKind() == zir::TypeKind::Int
                && symbol->returnType->getKind() == zir::TypeKind::Char) {
                stringIndexFunction_ = symbol;
            }
            module.symbol->members[funDecl->name_] = overloads;
            if (funDecl->visibility_ == Visibility::Public) {
                auto exportIt = module.symbol->exports.find(funDecl->name_);
                std::shared_ptr<OverloadSetSymbol> exportSet;
                if (exportIt == module.symbol->exports.end()) {
                    exportSet = std::make_shared<OverloadSetSymbol>(
                        funDecl->name_,
                        module.info->moduleName,
                        Visibility::Public
                    );
                    module.symbol->exports[funDecl->name_] = exportSet;
                } else {
                    exportSet = std::dynamic_pointer_cast<OverloadSetSymbol>(exportIt->second);
                }
                if (!exportSet) {
                    error(
                        funDecl->span,
                        "Function '" + funDecl->name_ + "' conflicts with an exported declaration."
                    );
                } else {
                    exportSet->addOverload(symbol);
                }
            }
        } else if (auto classDecl = dynamic_cast<ClassDecl*>(child.get())) {
            auto classSymbol =
                std::dynamic_pointer_cast<TypeSymbol>(module.scope->lookup(classDecl->name_));
            if (!classSymbol || !classSymbol->isClass
                || !classSymbol->genericParameterNames.empty()) {
                continue;
            }
            auto classType = std::static_pointer_cast<zir::ClassType>(classSymbol->type);
            auto& classInfo = classInfos_[classType->getCodegenName()];

            std::vector<std::shared_ptr<zir::ClassType>> classInterfaces;
            auto classBase = resolveClassImplementsList(*classDecl, classInterfaces);
            if (classBase) {
                bool hasOwnCtor = false;
                bool hasOwnDtor = false;
                for (const auto& methodDecl : classDecl->methods_) {
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
                    classInfo.methods.insert(
                        baseIt->second.methods.begin(),
                        baseIt->second.methods.end()
                    );
                    classInfo.nextVirtualSlot = baseIt->second.nextVirtualSlot;
                }
                for (const auto& conformance : classBase->getInterfaceConformances()) {
                    classType->addInterfaceConformance(conformance);
                }
            }

            for (const auto& methodDecl : classDecl->methods_) {
                if (methodDecl->isUnsafe_) {
                    ++unsafeTypeContextDepth_;
                }

                std::unordered_map<std::string, std::shared_ptr<zir::Type>> methodGenericBindings;
                for (const auto& genericParam : methodDecl->genericParams_) {
                    if (genericParam) {
                        auto placeholder = zir::makeGenericParameterType(genericParam->typeName);
                        methodGenericBindings[genericParam->typeName] = placeholder;
                    }
                }
                if (!methodGenericBindings.empty()) {
                    activeGenericBindingsStack_.push_back(methodGenericBindings);
                }

                std::vector<std::shared_ptr<VariableSymbol>> params;
                if (!methodDecl->isStatic_) {
                    params.push_back(
                        std::make_shared<VariableSymbol>(
                            "self",
                            classType,
                            BindingKind::Mutable,
                            false,
                            "self",
                            module.info->moduleName,
                            Visibility::Private
                        )
                    );
                    params.back()->syntaxName = methodDecl->syntaxName_;
                    params.back()->syntaxName.text = "self";
                    params.back()->syntaxName.occurrence.reset();
                }

                for (size_t i = 0; i < methodDecl->params_.size(); ++i) {
                    const auto& p = methodDecl->params_[i];
                    if (p->isRef && p->isSink) {
                        error(p->span, "Parameter cannot be passed by both 'ref' and 'sink'.");
                    }
                    if (p->isSink && p->isNoEscape) {
                        error(p->span, "A 'sink' parameter cannot have a 'noescape' contract.");
                    }
                    if (p->isVariadic && p->isSink) {
                        error(p->span, "Variadic parameter cannot be passed by 'sink'.");
                    }
                    if (p->isVariadic && p->isNoEscape) {
                        error(p->span, "Variadic parameter cannot have a 'noescape' contract.");
                    }
                    auto mappedType = mapType(*p->type);
                    if (!mappedType) {
                        error(p->span, "Unknown type: " + p->type->qualifiedName());
                        mappedType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
                    }
                    if (p->isNoEscape
                        && (p->isRef
                            || mappedType->getIntrinsicKind()
                                != zir::IntrinsicTypeKind::StringView)) {
                        error(
                            p->span,
                            "'noescape' currently requires a by-value StringView "
                            "parameter."
                        );
                    }
                    auto parameter = std::make_shared<VariableSymbol>(
                        p->name,
                        mappedType,
                        BindingKind::Mutable,
                        p->isRef,
                        p->name,
                        module.info->moduleName,
                        Visibility::Private
                    );
                    parameter->syntaxName = p->syntaxName;
                    parameter->is_sink = p->isSink;
                    parameter->is_noescape = p->isNoEscape;
                    params.push_back(std::move(parameter));
                }

                std::shared_ptr<zir::Type> retType;
                bool isCtor = methodDecl->name_ == "init";
                bool isDtor = methodDecl->name_ == "deinit";
                if (isDtor && (!methodDecl->params_.empty() || methodDecl->returnType_)) {
                    error(
                        methodDecl->span,
                        "Destructor 'deinit' cannot have parameters or a return type."
                    );
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
                    methodDecl->name_,
                    std::move(params),
                    std::move(retType),
                    "",
                    module.info->moduleName,
                    methodDecl->visibility_,
                    methodDecl->isUnsafe_
                );
                symbol->isEntryModule = module.info->isEntry;
                for (const auto& genericParam : methodDecl->genericParams_) {
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
                    methodDecl->resultBorrowSource_,
                    symbol->parameters,
                    symbol->returnType,
                    symbol->returnsRef,
                    methodDecl->span,
                    &methodDecl->resultBorrowName_
                );
                validateAndApplyFunctionAttributes(*methodDecl, symbol, false);
                if (symbol->isMethod && !symbol->isStatic && !symbol->isConstructor
                    && !symbol->isDestructor) {
                    symbol->vtableSlot = findOverriddenVtableSlot(classInfo, *symbol);
                    if (symbol->vtableSlot < 0) {
                        symbol->vtableSlot = classInfo.nextVirtualSlot++;
                    }
                }
                if (symbol->hasNoMangle || (symbol->hasExternC && symbol->externAbi == "C")) {
                    symbol->linkName = symbol->name;
                } else {
                    symbol->linkName = mangleName(
                        module.info->linkPath.empty() ? module.info->moduleId
                                                      : module.info->linkPath,
                        classDecl->name_ + "$" + methodDecl->name_ + "$"
                            + functionSignatureKey(*symbol)
                    );
                }
                declaredFunctionSymbols_[methodDecl.get()] = symbol;
                functionDeclarationNodes_[symbol.get()] = methodDecl.get();
                if (semanticInfo_) {
                    semanticInfo_->recordDeclaration(methodDecl.get(), symbol);
                }
                functionDeclarationModuleIds_[symbol.get()] = module.info->moduleId;
                functionGenericParamNames_[symbol.get()] = symbol->genericParameterNames;
                addClassMethodOverload(classInfo, symbol);
                if (isCtor) {
                    if (!classInfo.constructor) {
                        classInfo.constructor = symbol;
                    }
                } else if (isDtor) {
                    classInfo.destructor = symbol;
                }
            }

            bindInterfaceConformances(*classDecl, classType, classInfo, classInterfaces);
        } else if (auto interfaceDecl = dynamic_cast<InterfaceDecl*>(child.get())) {
            auto interfaceSymbol =
                std::dynamic_pointer_cast<TypeSymbol>(module.scope->lookup(interfaceDecl->name_));
            if (!interfaceSymbol || !interfaceSymbol->isInterface) {
                continue;
            }
            auto interfaceType = std::static_pointer_cast<zir::ClassType>(interfaceSymbol->type);
            auto& interfaceInfo = interfaceInfos_[interfaceType->getCodegenName()];

            std::vector<zir::ClassType::InterfaceMethod> methodMeta;
            for (const auto& methodDecl : interfaceDecl->methods_) {
                std::vector<std::shared_ptr<VariableSymbol>> params;
                params.push_back(
                    std::make_shared<VariableSymbol>(
                        "self",
                        interfaceType,
                        BindingKind::Mutable,
                        false,
                        "self",
                        module.info->moduleName,
                        Visibility::Private
                    )
                );
                params.back()->syntaxName = methodDecl->syntaxName_;
                params.back()->syntaxName.text = "self";
                params.back()->syntaxName.occurrence.reset();

                for (const auto& p : methodDecl->params_) {
                    auto mappedType = mapType(*p->type);
                    if (!mappedType) {
                        error(p->span, "Unknown type: " + p->type->qualifiedName());
                        mappedType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
                    }
                    auto parameter = std::make_shared<VariableSymbol>(
                        p->name,
                        mappedType,
                        BindingKind::Mutable,
                        p->isRef,
                        p->name,
                        module.info->moduleName,
                        Visibility::Private
                    );
                    parameter->syntaxName = p->syntaxName;
                    parameter->is_sink = p->isSink;
                    params.push_back(std::move(parameter));
                }

                std::shared_ptr<zir::Type> retType = methodDecl->returnType_
                    ? mapType(*methodDecl->returnType_)
                    : std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
                if (!retType) {
                    error(
                        methodDecl->span,
                        "Unknown return type in interface method '" + methodDecl->name_ + "'."
                    );
                    retType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
                }

                auto symbol = std::make_shared<FunctionSymbol>(
                    methodDecl->name_,
                    std::move(params),
                    std::move(retType),
                    "",
                    module.info->moduleName,
                    Visibility::Public
                );
                symbol->isMethod = true;
                symbol->ownerTypeCodegenName = interfaceType->getCodegenName();
                symbol->linkName = mangleName(
                    module.info->linkPath.empty() ? module.info->moduleId : module.info->linkPath,
                    interfaceDecl->name_ + "$" + methodDecl->name_ + "$"
                        + functionSignatureKey(*symbol)
                );

                if (interfaceInfo.methods.count(symbol->name)) {
                    error(
                        methodDecl->span,
                        "Interface method '" + methodDecl->name_ + "' already declared."
                    );
                    continue;
                }

                interfaceInfo.methods[symbol->name] = symbol;
                boundRoot_->externalFunctions.push_back(
                    std::make_unique<BoundExternalFunctionDeclaration>(symbol)
                );
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
        } else if (auto extDecl = dynamic_cast<ExtDecl*>(child.get())) {
            ++externTypeContextDepth_;
            std::vector<std::shared_ptr<VariableSymbol>> params;
            for (const auto& p : extDecl->params_) {
                if (p->isSink) {
                    error(p->span, "External function parameter cannot be passed by 'sink'.");
                }
                if (p->isVariadic && p->isNoEscape) {
                    error(p->span, "Variadic parameter cannot have a 'noescape' contract.");
                }
                if (p->isVariadic) {
                    error(
                        p->span,
                        "Variadic parameters are only supported in Zap "
                        "function declarations."
                    );
                }
                auto mappedType = mapType(*p->type);
                if (!mappedType) {
                    error(p->span, "Unknown type: " + p->type->qualifiedName());
                    mappedType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
                }
                if (p->isNoEscape
                    && (p->isRef
                        || mappedType->getIntrinsicKind() != zir::IntrinsicTypeKind::StringView)) {
                    error(
                        p->span,
                        "'noescape' currently requires a by-value StringView "
                        "parameter."
                    );
                }
                auto parameter = std::make_shared<VariableSymbol>(
                    p->name,
                    mappedType,
                    BindingKind::Mutable,
                    p->isRef,
                    p->name,
                    module.info->moduleName,
                    Visibility::Private
                );
                parameter->syntaxName = p->syntaxName;
                parameter->is_sink = p->isSink;
                parameter->is_noescape = p->isNoEscape;
                params.push_back(std::move(parameter));
            }

            auto retType = extDecl->returnType_
                ? mapType(*extDecl->returnType_)
                : std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
            if (!retType) {
                error(
                    extDecl->span,
                    "Unknown return type in external function '" + extDecl->name_ + "'."
                );
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
                extDecl->name_,
                std::move(params),
                std::move(retType),
                linkName,
                module.info->moduleName,
                extDecl->visibility_,
                false,
                extDecl->isCVariadic_
            );
            symbol->isExternal = true;
            symbol->isEntryModule = module.info->isEntry;
            symbol->resultBorrow = resolveResultBorrowContract(
                extDecl->resultBorrowSource_,
                symbol->parameters,
                symbol->returnType,
                false,
                extDecl->span,
                &extDecl->resultBorrowName_
            );
            validateAndApplyFunctionAttributes(*extDecl, symbol, true);
            if (semanticInfo_)
                semanticInfo_->recordDeclaration(extDecl, symbol);
            if (symbol->hasNoMangle || (symbol->hasExternC && symbol->externAbi == "C")) {
                symbol->linkName = symbol->name;
            }
            auto existing = module.scope->lookupLocal(extDecl->name_);
            if (findFunctionBySignature(existing, *symbol)) {
                error(
                    extDecl->span,
                    "External function '" + extDecl->name_ + "' already declared."
                );
                continue;
            }
            auto overloads = module.scope->declareFunction(extDecl->name_, symbol);
            if (!overloads) {
                error(
                    extDecl->span,
                    "External function '" + extDecl->name_
                        + "' conflicts with an existing declaration."
                );
                continue;
            }
            declaredFunctionSymbols_[extDecl] = symbol;
            module.symbol->members[extDecl->name_] = overloads;
            if (extDecl->visibility_ == Visibility::Public) {
                auto exportIt = module.symbol->exports.find(extDecl->name_);
                std::shared_ptr<OverloadSetSymbol> exportSet;
                if (exportIt == module.symbol->exports.end()) {
                    exportSet = std::make_shared<OverloadSetSymbol>(
                        extDecl->name_,
                        module.info->moduleName,
                        Visibility::Public
                    );
                    module.symbol->exports[extDecl->name_] = exportSet;
                } else {
                    exportSet = std::dynamic_pointer_cast<OverloadSetSymbol>(exportIt->second);
                }
                if (!exportSet) {
                    error(
                        extDecl->span,
                        "External function '" + extDecl->name_
                            + "' conflicts with an exported declaration."
                    );
                } else {
                    exportSet->addOverload(symbol);
                }
            }
        } else if (auto extensionDecl = dynamic_cast<ExtensionDecl*>(child.get())) {
            if (!extensionDecl->targetType_) {
                error(extensionDecl->span, "Extension declaration requires a target type.");
                continue;
            }
            if (extensionDecl->targetType_->isReference || extensionDecl->targetType_->isVarArgs
                || extensionDecl->targetType_->isWeak || extensionDecl->targetType_->isFailable
                || extensionDecl->targetType_->isFunPtr) {
                error(
                    extensionDecl->targetType_->span,
                    "This extension target form is not supported yet."
                );
                continue;
            }

            std::unordered_map<std::string, std::shared_ptr<zir::Type>> genericBindings;
            bool validGenericParameters = true;
            for (const auto& genericParam : extensionDecl->genericParams_) {
                if (!genericParam) {
                    continue;
                }
                const auto [_, inserted] = genericBindings.emplace(
                    genericParam->typeName,
                    zir::makeGenericParameterType(genericParam->typeName)
                );
                if (!inserted) {
                    error(
                        genericParam->span,
                        "Duplicate generic parameter '" + genericParam->typeName
                            + "' in extension declaration."
                    );
                    validGenericParameters = false;
                }
            }
            if (!validGenericParameters) {
                continue;
            }
            if (!genericBindings.empty()) {
                activeGenericBindingsStack_.push_back(genericBindings);
            }

            auto targetType = mapType(*extensionDecl->targetType_);
            if (!targetType) {
                error(
                    extensionDecl->targetType_->span,
                    "Unknown extension target type '" + extensionDecl->targetType_->qualifiedName()
                        + "'."
                );
                if (!genericBindings.empty()) {
                    activeGenericBindingsStack_.pop_back();
                }
                continue;
            }
            const auto isBareGenericTarget = [&]() {
                if (targetType->getKind() != zir::TypeKind::Record) {
                    return false;
                }
                const auto targetRecord = std::static_pointer_cast<zir::RecordType>(targetType);
                return targetRecord->getRole() == zir::RecordRole::GenericParameter;
            };
            if (isBareGenericTarget()) {
                error(
                    extensionDecl->targetType_->span,
                    "Blanket extensions must be anchored in a concrete type "
                    "constructor."
                );
                if (!genericBindings.empty()) {
                    activeGenericBindingsStack_.pop_back();
                }
                continue;
            }
            bool hasUnboundGenericParameter = false;
            for (const auto& [name, _] : genericBindings) {
                if (!containsGenericParameter(targetType, name)) {
                    error(
                        extensionDecl->targetType_->span,
                        "Generic extension parameter '" + name + "' must appear in its target type."
                    );
                    hasUnboundGenericParameter = true;
                }
            }
            if (hasUnboundGenericParameter) {
                activeGenericBindingsStack_.pop_back();
                continue;
            }
            if (targetType->getKind() == zir::TypeKind::Void) {
                error(extensionDecl->targetType_->span, "Cannot declare an extension for Void.");
                if (!genericBindings.empty()) {
                    activeGenericBindingsStack_.pop_back();
                }
                continue;
            }
            if (targetType->getKind() == zir::TypeKind::Class
                && std::static_pointer_cast<zir::ClassType>(targetType)->isInterface()) {
                error(extensionDecl->targetType_->span, "Interfaces cannot be extended yet.");
                if (!genericBindings.empty()) {
                    activeGenericBindingsStack_.pop_back();
                }
                continue;
            }

            for (const auto& methodDecl : extensionDecl->methods_) {
                if (!methodDecl) {
                    continue;
                }
                if (methodDecl->isStatic_) {
                    error(methodDecl->span, "Static extension methods are not supported yet.");
                    continue;
                }
                const bool receiverIsRef =
                    methodDecl->extensionReceiverMode_ == ExtensionReceiverMode::Ref;
                if (receiverIsRef && targetType->getKind() == zir::TypeKind::Record
                    && std::static_pointer_cast<zir::RecordType>(targetType)
                        ->hasImmutableFields()) {
                    error(
                        methodDecl->extensionReceiverSpan_,
                        "'ref self' cannot be used with an immutable record target."
                    );
                    continue;
                }
                if (!methodDecl->genericParams_.empty()) {
                    error(methodDecl->span, "Generic extension methods are not supported yet.");
                    continue;
                }
                if (methodDecl->name_ == "init" || methodDecl->name_ == "deinit") {
                    error(
                        methodDecl->span,
                        "Extension methods cannot declare '" + methodDecl->name_ + "'."
                    );
                    continue;
                }

                std::vector<std::shared_ptr<VariableSymbol>> params;
                params.push_back(
                    std::make_shared<VariableSymbol>(
                        "self",
                        targetType,
                        BindingKind::Mutable,
                        receiverIsRef,
                        "self",
                        module.info->moduleName,
                        Visibility::Private
                    )
                );
                params.back()->syntaxName = methodDecl->syntaxName_;
                params.back()->syntaxName.text = "self";
                params.back()->syntaxName.occurrence.reset();
                bool valid = true;
                for (size_t i = 0; i < methodDecl->params_.size(); ++i) {
                    const auto& parameterDecl = methodDecl->params_[i];
                    if (parameterDecl->isRef && parameterDecl->isSink) {
                        error(
                            parameterDecl->span,
                            "Parameter cannot be passed by both 'ref' and 'sink'."
                        );
                        valid = false;
                    }
                    if (parameterDecl->isSink && parameterDecl->isNoEscape) {
                        error(
                            parameterDecl->span,
                            "A 'sink' parameter cannot have a 'noescape' contract."
                        );
                        valid = false;
                    }
                    if (parameterDecl->isVariadic && i + 1 != methodDecl->params_.size()) {
                        error(
                            parameterDecl->span,
                            "Variadic parameter must be the last parameter."
                        );
                        valid = false;
                    }
                    if (parameterDecl->isVariadic && parameterDecl->isRef) {
                        error(parameterDecl->span, "Variadic parameter cannot be passed by 'ref'.");
                        valid = false;
                    }
                    if (parameterDecl->isVariadic && parameterDecl->isSink) {
                        error(
                            parameterDecl->span,
                            "Variadic parameter cannot be passed by 'sink'."
                        );
                        valid = false;
                    }
                    if (parameterDecl->isVariadic && parameterDecl->isNoEscape) {
                        error(
                            parameterDecl->span,
                            "Variadic parameter cannot have a 'noescape' contract."
                        );
                        valid = false;
                    }

                    auto parameterType = mapType(*parameterDecl->type);
                    if (!parameterType) {
                        error(
                            parameterDecl->span,
                            "Unknown type: " + parameterDecl->type->qualifiedName()
                        );
                        valid = false;
                        parameterType = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
                    }
                    if (parameterDecl->isNoEscape
                        && (parameterDecl->isRef
                            || parameterType->getIntrinsicKind()
                                != zir::IntrinsicTypeKind::StringView)) {
                        error(
                            parameterDecl->span,
                            "'noescape' currently requires a by-value StringView "
                            "parameter."
                        );
                        valid = false;
                    }
                    auto parameter = std::make_shared<VariableSymbol>(
                        parameterDecl->name,
                        parameterType,
                        BindingKind::Mutable,
                        parameterDecl->isRef,
                        parameterDecl->name,
                        module.info->moduleName,
                        Visibility::Private
                    );
                    parameter->syntaxName = parameterDecl->syntaxName;
                    parameter->is_sink = parameterDecl->isSink;
                    parameter->is_noescape = parameterDecl->isNoEscape;
                    if (parameterDecl->isVariadic) {
                        parameter->is_variadic_pack = true;
                        parameter->variadic_element_type = parameterType;
                        parameter->type = makeVariadicViewType(parameterType);
                    }
                    params.push_back(std::move(parameter));
                }
                if (!valid) {
                    continue;
                }

                auto returnType = methodDecl->returnType_
                    ? mapType(*methodDecl->returnType_)
                    : std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
                if (!returnType) {
                    error(
                        methodDecl->span,
                        "Unknown return type in extension method '" + methodDecl->name_ + "'."
                    );
                    continue;
                }

                auto symbol = std::make_shared<FunctionSymbol>(
                    methodDecl->name_,
                    std::move(params),
                    std::move(returnType),
                    "",
                    module.info->moduleName,
                    methodDecl->visibility_,
                    methodDecl->isUnsafe_
                );
                symbol->isExtensionMethod = true;
                symbol->extensionTargetType = targetType;
                symbol->extensionDeclaringModuleId = module.info->moduleId;
                symbol->isEntryModule = module.info->isEntry;
                symbol->returnsRef = methodDecl->returnsRef_;
                for (const auto& genericParam : extensionDecl->genericParams_) {
                    if (genericParam) {
                        symbol->genericParameterNames.push_back(genericParam->typeName);
                    }
                }
                symbol->resultBorrow = resolveResultBorrowContract(
                    methodDecl->resultBorrowSource_,
                    symbol->parameters,
                    symbol->returnType,
                    symbol->returnsRef,
                    methodDecl->span,
                    &methodDecl->resultBorrowName_
                );
                validateAndApplyFunctionAttributes(*methodDecl, symbol, false);
                if (symbol->hasNoMangle || (symbol->hasExternC && symbol->externAbi == "C")) {
                    symbol->linkName = symbol->name;
                } else {
                    symbol->linkName = mangleName(
                        module.info->linkPath.empty() ? module.info->moduleId
                                                      : module.info->linkPath,
                        "extend$" + typeInterner_.mangleKey(targetType) + "$" + methodDecl->name_
                            + "$" + functionSignatureKey(*symbol)
                    );
                }

                bool duplicateInModule = false;
                const auto isDuplicate = [&](const std::shared_ptr<FunctionSymbol>& candidate) {
                    if (!candidate
                        || candidate->extensionDeclaringModuleId != module.info->moduleId) {
                        return false;
                    }
                    return genericBindings.empty()
                        ? sameExtensionCallSignature(*candidate, *symbol)
                        : sameGenericExtensionSignature(*candidate, *symbol);
                };
                if (genericBindings.empty()) {
                    const auto targetKey = typeInterner_.mangleKey(targetType);
                    const auto targetIt = extensionInfos_.find(targetKey);
                    const auto existing = targetIt == extensionInfos_.end()
                        ? nullptr
                        : [&]() -> std::shared_ptr<Symbol> {
                        const auto methodIt = targetIt->second.methods.find(methodDecl->name_);
                        return methodIt == targetIt->second.methods.end() ? nullptr
                                                                          : methodIt->second;
                    }();
                    for (const auto& candidate : collectOverloads(existing)) {
                        if (isDuplicate(candidate)) {
                            duplicateInModule = true;
                            break;
                        }
                    }
                } else {
                    for (const auto& extensionInfo : genericExtensionInfos_) {
                        const auto methodIt = extensionInfo.methods.find(methodDecl->name_);
                        if (methodIt == extensionInfo.methods.end()) {
                            continue;
                        }
                        for (const auto& candidate : collectOverloads(methodIt->second)) {
                            if (isDuplicate(candidate)) {
                                duplicateInModule = true;
                                break;
                            }
                        }
                        if (duplicateInModule) {
                            break;
                        }
                    }
                }
                if (duplicateInModule) {
                    error(
                        methodDecl->span,
                        "Extension method '" + methodDecl->name_ + "' is already declared for '"
                            + renderTypeForUser(targetType) + "'."
                    );
                    continue;
                }

                addExtensionMethodOverload(targetType, symbol);
                declaredFunctionSymbols_[methodDecl.get()] = symbol;
                functionDeclarationNodes_[symbol.get()] = methodDecl.get();
                functionDeclarationModuleIds_[symbol.get()] = module.info->moduleId;
                functionGenericParamNames_[symbol.get()] = symbol->genericParameterNames;
                extensionDeclarationNodes_[symbol.get()] = extensionDecl;
                if (semanticInfo_) {
                    semanticInfo_->recordDeclaration(methodDecl.get(), symbol);
                }
            }
            if (!genericBindings.empty()) {
                activeGenericBindingsStack_.pop_back();
            }
        } else if (auto bindingDecl = dynamic_cast<BindingDecl*>(child.get())) {
            const bool isConstant = bindingDecl->kind_ == BindingKind::CompileTimeConstant;
            if (!isConstant && !bindingDecl->isGlobal_) {
                continue;
            }
            std::shared_ptr<zir::Type> type;
            if (bindingDecl->type_) {
                type = mapType(*bindingDecl->type_);
                if (!type) {
                    error(
                        bindingDecl->span,
                        "Unknown type: " + bindingDecl->type_->qualifiedName()
                    );
                    type = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
                }
            } else {
                type = std::make_shared<zir::PrimitiveType>(zir::TypeKind::Void);
            }
            auto linkName = bindingDecl->isExternal_
                ? bindingDecl->name_
                : mangleName(
                      module.info->linkPath.empty() ? module.info->moduleId : module.info->linkPath,
                      bindingDecl->name_
                  );
            auto symbol = std::make_shared<VariableSymbol>(
                bindingDecl->name_,
                type,
                bindingDecl->kind_,
                false,
                linkName,
                module.info->moduleName,
                bindingDecl->visibility_
            );
            symbol->is_external = bindingDecl->isExternal_;
            if (!module.scope->declare(bindingDecl->name_, symbol)) {
                if (isConstant) {
                    error(
                        bindingDecl->span,
                        "Identifier '" + bindingDecl->name_ + "' already declared."
                    );
                } else {
                    error(
                        bindingDecl->span,
                        "Variable '" + bindingDecl->name_ + "' already declared."
                    );
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
