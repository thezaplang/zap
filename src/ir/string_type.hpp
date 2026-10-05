#pragma once

#include "type.hpp"
#include <memory>

namespace zir {

inline std::shared_ptr<RecordType> makeStringType() {
    return std::make_shared<RecordType>("String", "String", IntrinsicTypeKind::String);
}

inline std::shared_ptr<RecordType> makeStringViewType() {
    return std::make_shared<RecordType>("StringView", "StringView", IntrinsicTypeKind::StringView);
}

inline bool isIntrinsicStringType(const Type& type) {
    return type.getIntrinsicKind() == IntrinsicTypeKind::String
        || type.getIntrinsicKind() == IntrinsicTypeKind::StringView;
}

inline bool isIntrinsicStringType(const std::shared_ptr<Type>& type) {
    return type && isIntrinsicStringType(*type);
}

inline bool isIntrinsicStringViewType(const Type& type) {
    return type.getIntrinsicKind() == IntrinsicTypeKind::StringView;
}

inline bool isIntrinsicStringViewType(const std::shared_ptr<Type>& type) {
    return type && isIntrinsicStringViewType(*type);
}

} // namespace zir
