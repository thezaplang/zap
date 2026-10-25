#include "type_layout.hpp"
#include "../ir/string_type.hpp"
#include <algorithm>

namespace sema {
namespace {

uint64_t alignTo(uint64_t value, uint64_t alignment) {
    if (alignment <= 1) {
        return value;
    }
    return ((value + alignment - 1) / alignment) * alignment;
}

TypeLayout layoutOfAggregateField(const std::shared_ptr<zir::Type>& type, TargetInfo targetInfo) {
    if (type && type->getKind() == zir::TypeKind::Void) {
        return {1, 1};
    }
    return computeTypeLayout(type, targetInfo);
}

TypeLayout layoutOfRecord(const zir::RecordType& record, TargetInfo targetInfo) {
    if (zir::isIntrinsicStringType(record)) {
        uint64_t wordSize = targetInfo.pointerBitWidth / 8;
        uint64_t lengthAlign = std::min<uint64_t>(8, wordSize);
        uint64_t offset = alignTo(wordSize, lengthAlign) + 8;
        uint64_t alignment = std::max(wordSize, lengthAlign);
        return {alignTo(offset, alignment), alignment};
    }

    uint64_t offset = 0;
    uint64_t maxAlign = 1;
    for (const auto& field : record.getFields()) {
        auto fieldLayout = layoutOfAggregateField(field.type, targetInfo);
        maxAlign = std::max(maxAlign, fieldLayout.align);
        if (!record.isPacked) {
            offset = alignTo(offset, fieldLayout.align);
        }
        offset += fieldLayout.size;
    }

    if (!record.isPacked) {
        offset = alignTo(offset, maxAlign);
    }
    return {offset, record.isPacked ? 1 : maxAlign};
}

TypeLayout layoutOfTaggedUnion(const zir::TaggedUnionType& taggedUnion, TargetInfo targetInfo) {
    TypeLayout payloadLayout{1, 1};
    for (const auto& variant : taggedUnion.getVariants()) {
        if (!variant.payloadType) {
            continue;
        }
        auto candidate = layoutOfAggregateField(variant.payloadType, targetInfo);
        if (candidate.size > payloadLayout.size
            || (candidate.size == payloadLayout.size && candidate.align > payloadLayout.align)) {
            payloadLayout = candidate;
        }
    }

    uint64_t offset = 4;
    const uint64_t structAlign = std::max<uint64_t>(4, payloadLayout.align);
    offset = alignTo(offset, payloadLayout.align);
    offset += payloadLayout.size;
    return {alignTo(offset, structAlign), structAlign};
}

} // namespace

TypeLayout computeTypeLayout(const std::shared_ptr<zir::Type>& type, TargetInfo targetInfo) {
    if (!type) {
        return {0, 1};
    }

    switch (type->getKind()) {
        case zir::TypeKind::Void:
            return {0, 1};
        case zir::TypeKind::Bool:
        case zir::TypeKind::Char:
        case zir::TypeKind::Int8:
        case zir::TypeKind::UInt8:
            return {1, 1};
        case zir::TypeKind::Int16:
        case zir::TypeKind::UInt16:
        case zir::TypeKind::Int32:
        case zir::TypeKind::UInt32:
        case zir::TypeKind::Float:
        case zir::TypeKind::Float32:
        case zir::TypeKind::Int:
        case zir::TypeKind::UInt:
        case zir::TypeKind::Int64:
        case zir::TypeKind::UInt64:
        case zir::TypeKind::Float64: {
            auto info = *zir::numericTypeInfo(type->getKind());
            uint64_t size = info.bitWidth(targetInfo.nativeIntegerBitWidth()) / 8;
            return {size, std::min<uint64_t>(size, targetInfo.pointerBitWidth / 8)};
        }
        case zir::TypeKind::Pointer:
        case zir::TypeKind::NullPtr:
        case zir::TypeKind::FunctionPointer:
        case zir::TypeKind::Class: {
            uint64_t size = targetInfo.pointerBitWidth / 8;
            return {size, size};
        }
        case zir::TypeKind::Enum:
            if (std::static_pointer_cast<zir::EnumType>(type)->hasReprC) {
                return {4, 4};
            }
            {
                uint64_t size = targetInfo.nativeIntegerBitWidth() / 8;
                return {size, size};
            }
        case zir::TypeKind::Record:
            return layoutOfRecord(*std::static_pointer_cast<zir::RecordType>(type), targetInfo);
        case zir::TypeKind::TaggedUnion:
            return layoutOfTaggedUnion(
                *std::static_pointer_cast<zir::TaggedUnionType>(type),
                targetInfo
            );
        case zir::TypeKind::Array: {
            auto array = std::static_pointer_cast<zir::ArrayType>(type);
            auto elementLayout = layoutOfAggregateField(array->getBaseType(), targetInfo);
            return {elementLayout.size * array->getSize(), elementLayout.align};
        }
    }

    return {0, 1};
}

} // namespace sema
