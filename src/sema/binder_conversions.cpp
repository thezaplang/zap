#include "binder.hpp"

namespace sema {

int Binder::typeBitWidth(std::shared_ptr<zir::Type> type) const {
    if (!type) {
        return 0;
    }
    if (type->getKind() == zir::TypeKind::Bool) {
        return 1;
    }
    if (type->getKind() == zir::TypeKind::Char) {
        return 8;
    }
    auto info = zir::numericTypeInfo(type->getKind());
    return info ? info->bitWidth(targetInfo_.nativeIntegerBitWidth()) : 0;
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

void Binder::requireUnsafeEnabled(SourceSpan span, const std::string& feature) {
    if (!allowUnsafe_) {
        error(span, "Using " + feature + " requires '--allow-unsafe'.");
    }
}

void Binder::requireUnsafeContext(SourceSpan span, const std::string& feature) {
    if (!isUnsafeActive()) {
        error(span, "Using " + feature + " is only allowed inside unsafe code.");
    }
}

std::shared_ptr<zir::Type> Binder::getCVariadicArgumentType(std::shared_ptr<zir::Type> type) {
    if (!type)
        return nullptr;

    if (isStringType(type)) {
        return std::make_shared<zir::PointerType>(
            std::make_shared<zir::PrimitiveType>(zir::TypeKind::Char)
        );
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
            return std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int32);
        case zir::TypeKind::Int32:
        case zir::TypeKind::Int64:
        case zir::TypeKind::UInt32:
        case zir::TypeKind::UInt64:
        case zir::TypeKind::Int:
        case zir::TypeKind::UInt:
        case zir::TypeKind::Float64:
            return type;
        case zir::TypeKind::Void:
        case zir::TypeKind::Pointer:
        case zir::TypeKind::NullPtr:
        case zir::TypeKind::Record:
        case zir::TypeKind::Class:
        case zir::TypeKind::Array:
        case zir::TypeKind::Enum:
        case zir::TypeKind::TaggedUnion:
        case zir::TypeKind::FunctionPointer:
            return nullptr;
    }

    return nullptr;
}

std::unique_ptr<BoundExpression> Binder::applyConversion(
    std::unique_ptr<BoundExpression> expr,
    const Conversion& conversion
) {
    if (!expr || !conversion.targetType)
        return expr;
    if (!conversion.requiresCast()) {
        return expr;
    }
    return std::make_unique<BoundCast>(std::move(expr), conversion.targetType);
}

} // namespace sema
