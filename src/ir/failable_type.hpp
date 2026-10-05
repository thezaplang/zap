#pragma once

#include "type.hpp"
#include <memory>
#include <optional>

namespace zir {

struct FailableTypeLayout {
    static constexpr int OkField = 0;
    static constexpr int ValueField = 1;
    static constexpr int ErrorField = 2;

    std::shared_ptr<Type> valueType;
    std::shared_ptr<Type> errorType;
};

std::optional<FailableTypeLayout> getFailableTypeLayout(const std::shared_ptr<Type>& type);
std::shared_ptr<RecordType> makeFailableRecordType(
    const std::shared_ptr<Type>& valueType,
    const std::shared_ptr<Type>& errorType
);

} // namespace zir
