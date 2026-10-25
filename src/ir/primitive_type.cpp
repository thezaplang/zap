#include "type.hpp"

namespace zir {

std::optional<NumericTypeInfo> numericTypeInfo(TypeKind kind) {
    switch (kind) {
        case TypeKind::Int8:
            return NumericTypeInfo{NumericCategory::SignedInteger, 8, false};
        case TypeKind::Int16:
            return NumericTypeInfo{NumericCategory::SignedInteger, 16, false};
        case TypeKind::Int32:
            return NumericTypeInfo{NumericCategory::SignedInteger, 32, false};
        case TypeKind::Int64:
            return NumericTypeInfo{NumericCategory::SignedInteger, 64, false};
        case TypeKind::UInt8:
            return NumericTypeInfo{NumericCategory::UnsignedInteger, 8, false};
        case TypeKind::UInt16:
            return NumericTypeInfo{NumericCategory::UnsignedInteger, 16, false};
        case TypeKind::UInt32:
            return NumericTypeInfo{NumericCategory::UnsignedInteger, 32, false};
        case TypeKind::UInt64:
            return NumericTypeInfo{NumericCategory::UnsignedInteger, 64, false};
        case TypeKind::Int:
            return NumericTypeInfo{NumericCategory::SignedInteger, 0, true};
        case TypeKind::UInt:
            return NumericTypeInfo{NumericCategory::UnsignedInteger, 0, true};
        case TypeKind::Float:
        case TypeKind::Float32:
            return NumericTypeInfo{NumericCategory::FloatingPoint, 32, false};
        case TypeKind::Float64:
            return NumericTypeInfo{NumericCategory::FloatingPoint, 64, false};
        default:
            return std::nullopt;
    }
}

TypeKind canonicalPrimitiveKind(TypeKind kind) {
    return kind == TypeKind::Float ? TypeKind::Float32 : kind;
}

std::string_view primitiveIrName(TypeKind kind) {
    switch (kind) {
        case TypeKind::Int8:
            return "i8";
        case TypeKind::Int16:
            return "i16";
        case TypeKind::Int32:
            return "i32";
        case TypeKind::Int64:
            return "i64";
        case TypeKind::UInt8:
            return "u8";
        case TypeKind::UInt16:
            return "u16";
        case TypeKind::UInt32:
            return "u32";
        case TypeKind::UInt64:
            return "u64";
        case TypeKind::Int:
            return "isize";
        case TypeKind::UInt:
            return "usize";
        case TypeKind::Float:
        case TypeKind::Float32:
            return "f32";
        case TypeKind::Float64:
            return "f64";
        case TypeKind::Bool:
            return "i1";
        case TypeKind::Char:
            return "i8";
        case TypeKind::Void:
            return "void";
        case TypeKind::NullPtr:
            return "null";
        default:
            return "unknown";
    }
}

std::string PrimitiveType::toString() const {
    return std::string(primitiveIrName(kind));
}

} // namespace zir
