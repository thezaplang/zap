#ifndef ZAP_RUNTIME_STRING_INTERNAL_H
#define ZAP_RUNTIME_STRING_INTERNAL_H

#include "string_layout.h"

#include <stddef.h>

#if defined(__GNUC__) || defined(__clang__)
#define ZAP_RUNTIME_INTERNAL __attribute__((visibility("hidden")))
#else
#define ZAP_RUNTIME_INTERNAL
#endif

ZAP_RUNTIME_INTERNAL char* zap_string_alloc_owned(size_t len);
ZAP_RUNTIME_INTERNAL void zap_string_release_ptr(const char* ptr);
ZAP_RUNTIME_INTERNAL char* zap_string_to_cstr(zap_string_t s);
zap_string_t zap_string_from_cstr(const char* cstr);
zap_string_t zap_string_from_ptrlen(const char* ptr, long len);

#undef ZAP_RUNTIME_INTERNAL

#endif
