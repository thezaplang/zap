#include "binder.hpp"

namespace sema {

void Binder::visit(MemberAccessNode& node) {
    node.left_->accept(*this);
    if (expressionStack_.empty())
        return;

    auto left = std::move(expressionStack_.top());
    expressionStack_.pop();

    if (auto moduleRef = dynamic_cast<BoundModuleReference*>(left.get())) {
        auto memberIt = moduleRef->symbol->exports.find(node.member_);
        if (memberIt == moduleRef->symbol->exports.end()) {
            auto privateIt = moduleRef->symbol->members.find(node.member_);
            if (privateIt != moduleRef->symbol->members.end()) {
                error(
                    node.span,
                    "Member '" + node.member_ + "' of module '" + moduleRef->symbol->name
                        + "' is private."
                );
            } else {
                error(
                    node.span,
                    "Module '" + moduleRef->symbol->name + "' has no member '" + node.member_ + "'"
                );
            }
            return;
        }

        recordModuleMemberName(node, memberIt->second);
        if (auto varSymbol = std::dynamic_pointer_cast<VariableSymbol>(memberIt->second)) {
            expressionStack_.push(std::make_unique<BoundVariableExpression>(varSymbol));
            return;
        }
        if (auto typeSymbol = std::dynamic_pointer_cast<TypeSymbol>(memberIt->second)) {
            expressionStack_.push(std::make_unique<BoundLiteral>("", typeSymbol->type, true));
            return;
        }
        if (auto nestedModule = std::dynamic_pointer_cast<ModuleSymbol>(memberIt->second)) {
            expressionStack_.push(std::make_unique<BoundModuleReference>(nestedModule));
            return;
        }

        error(node.span, "'" + node.member_ + "' is not a value or type.");
        return;
    }

    if (left->type->getKind() == zir::TypeKind::Enum) {
        auto enumType = std::static_pointer_cast<zir::EnumType>(left->type);
        int64_t value = enumType->getVariantDiscriminant(node.member_);
        if (value != -1) {
            expressionStack_.push(std::make_unique<BoundLiteral>(std::to_string(value), enumType));
            return;
        }
    } else if (left->type->getKind() == zir::TypeKind::TaggedUnion) {
        auto taggedUnionType = std::static_pointer_cast<zir::TaggedUnionType>(left->type);
        if (node.member_ == "tag") {
            expressionStack_.push(
                std::make_unique<BoundMemberAccess>(
                    std::move(left),
                    node.member_,
                    std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int32)
                )
            );
            return;
        }
        if (taggedUnionType->findVariant(node.member_) && dynamic_cast<BoundLiteral*>(left.get())) {
            expressionStack_.push(
                std::make_unique<BoundMemberAccess>(std::move(left), node.member_, taggedUnionType)
            );
            return;
        }
    } else if (left->type->getKind() == zir::TypeKind::Record) {
        auto recordType = std::static_pointer_cast<zir::RecordType>(left->type);
        for (const auto& field : recordType->getFields()) {
            if (field.name == node.member_) {
                expressionStack_.push(
                    std::make_unique<BoundMemberAccess>(std::move(left), node.member_, field.type)
                );
                return;
            }
        }
    } else if (left->type->getKind() == zir::TypeKind::Pointer) {
        auto ptrType = std::static_pointer_cast<zir::PointerType>(left->type);
        auto baseType = ptrType->getBaseType();

        if (baseType->getKind() == zir::TypeKind::Record) {
            auto recordType = std::static_pointer_cast<zir::RecordType>(baseType);
            for (const auto& field : recordType->getFields()) {
                if (field.name == node.member_) {
                    expressionStack_.push(
                        std::make_unique<BoundMemberAccess>(
                            std::move(left),
                            node.member_,
                            field.type
                        )
                    );
                    return;
                }
            }
        } else if (baseType->getKind() == zir::TypeKind::Class) {
            auto classType = std::static_pointer_cast<zir::ClassType>(baseType);
            if (classType->isWeak()) {
                error(node.span, "Weak references cannot be accessed directly.");
                return;
            }
            auto infoIt = classInfos_.find(classType->getCodegenName());
            if (infoIt != classInfos_.end()) {
                auto fieldIt = infoIt->second.fields.find(node.member_);
                if (fieldIt != infoIt->second.fields.end()) {
                    auto fieldVis = fieldIt->second->visibility;
                    bool allowed = fieldVis == Visibility::Public
                        || (!currentClassStack_.empty()
                            && currentClassStack_.back() == classType->getName())
                        || (fieldVis == Visibility::Protected && !currentClassStack_.empty());
                    if (!allowed) {
                        error(node.span, "Field '" + node.member_ + "' is not accessible.");
                        return;
                    }
                    expressionStack_.push(
                        std::make_unique<BoundMemberAccess>(
                            std::move(left),
                            node.member_,
                            fieldIt->second->type
                        )
                    );
                    return;
                }
            }
        }
    } else if (left->type->getKind() == zir::TypeKind::Class) {
        auto classType = std::static_pointer_cast<zir::ClassType>(left->type);
        if (classType->isWeak()) {
            error(node.span, "Weak references cannot be accessed directly.");
            return;
        }
        auto infoIt = classInfos_.find(classType->getCodegenName());
        if (infoIt != classInfos_.end()) {
            auto fieldIt = infoIt->second.fields.find(node.member_);
            if (fieldIt != infoIt->second.fields.end()) {
                auto fieldVis = fieldIt->second->visibility;
                bool allowed = fieldVis == Visibility::Public
                    || (!currentClassStack_.empty()
                        && currentClassStack_.back() == classType->getName())
                    || (fieldVis == Visibility::Protected && !currentClassStack_.empty());
                if (!allowed) {
                    error(node.span, "Field '" + node.member_ + "' is not accessible.");
                    return;
                }
                expressionStack_.push(
                    std::make_unique<BoundMemberAccess>(
                        std::move(left),
                        node.member_,
                        fieldIt->second->type
                    )
                );
                return;
            }
        }
    }

    error(
        node.span,
        "Member '" + node.member_ + "' not found in type '" + renderTypeForUser(left->type) + "'"
    );
}

} // namespace sema
