#pragma once

#include "../ir/type.hpp"
#include "target_info.hpp"
#include <cstdint>
#include <memory>

namespace sema {

struct TypeLayout {
    uint64_t size = 0;
    uint64_t align = 1;
};

TypeLayout computeTypeLayout(const std::shared_ptr<zir::Type>& type, TargetInfo targetInfo = {});

} // namespace sema
