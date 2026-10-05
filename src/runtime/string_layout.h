#pragma once

#include <stdint.h>

// Shared String ABI. A non-empty owned String points immediately after this
// header. StringView never participates in this layout.
#define ZAP_STRING_REFCOUNT_INDEX 0
#define ZAP_STRING_LENGTH_INDEX 1
#define ZAP_STRING_DATA_INDEX 2
#define ZAP_STRING_IMMORTAL_REFCOUNT INT64_MIN

typedef struct {
    int64_t refs;
    int64_t len;
} zap_string_header_t;

typedef struct {
    const char* ptr;
    int64_t len;
} zap_string_t;
