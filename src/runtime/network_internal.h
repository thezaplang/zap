#ifndef ZAP_RUNTIME_NETWORK_INTERNAL_H
#define ZAP_RUNTIME_NETWORK_INTERNAL_H

#include "string_layout.h"

long netConnect(zap_string_t host, long port);

#if defined(__GNUC__) || defined(__clang__)
#define ZAP_RUNTIME_INTERNAL __attribute__((visibility("hidden")))
#else
#define ZAP_RUNTIME_INTERNAL
#endif

ZAP_RUNTIME_INTERNAL char* zap_network_copy_path(zap_string_t path);

#undef ZAP_RUNTIME_INTERNAL

#endif
