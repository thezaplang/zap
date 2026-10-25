#pragma once

#include <cstdint>

namespace sema {

struct TargetInfo {
    uint16_t pointerBitWidth = sizeof(void*) * 8;

    uint16_t nativeIntegerBitWidth() const { return pointerBitWidth; }
};

} // namespace sema
