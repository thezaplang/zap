#include "ir/failable_type.hpp"
#include "ir/type_identity.hpp"

namespace zir {
namespace {

std::shared_ptr<RecordType> asFailableRecord(const std::shared_ptr<Type>& type) {
    if (!type || type->getKind() != TypeKind::Record) {
        return nullptr;
    }

    auto record = std::static_pointer_cast<RecordType>(type);
    if (record->getRole() != RecordRole::Failable) {
        return nullptr;
    }

    const auto& fields = record->getFields();
    if (fields.size() != 3 || fields[FailableTypeLayout::OkField].name != "ok"
        || fields[FailableTypeLayout::ValueField].name != "value"
        || fields[FailableTypeLayout::ErrorField].name != "error") {
        return nullptr;
    }

    return record;
}

} // namespace

std::optional<FailableTypeLayout> getFailableTypeLayout(const std::shared_ptr<Type>& type) {
    auto record = asFailableRecord(type);
    if (!record) {
        return std::nullopt;
    }
    return FailableTypeLayout{record->getFields()[FailableTypeLayout::ValueField].type,
        record->getFields()[FailableTypeLayout::ErrorField].type};
}

std::shared_ptr<RecordType> makeFailableRecordType(
    const std::shared_ptr<Type>& valueType,
    const std::shared_ptr<Type>& errorType
) {
    auto suffix = (valueType ? typeMangleKey(valueType) : "missing") + std::string("$")
        + (errorType ? typeMangleKey(errorType) : "missing");
    auto typeName = std::string("failable$") + suffix;
    auto type = std::make_shared<RecordType>(
        typeName,
        typeName,
        IntrinsicTypeKind::None,
        RecordRole::Failable
    );
    type->addField("ok", std::make_shared<PrimitiveType>(TypeKind::Bool));
    type->addField("value", valueType);
    type->addField("error", errorType);
    return type;
}

} // namespace zir
