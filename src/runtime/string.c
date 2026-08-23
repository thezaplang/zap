#include "allocation_internal.h"
#include "arc_layout.h"
#include "string_internal.h"
#include "string_layout.h"

#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(offsetof(zap_string_header_t, refs) == 0,
               "String ABI: refs offset mismatch");
_Static_assert(offsetof(zap_string_header_t, len) == sizeof(int64_t),
               "String ABI: len offset mismatch");
_Static_assert(sizeof(zap_string_header_t) == 2 * sizeof(int64_t),
               "String ABI: unexpected header padding");
_Static_assert(offsetof(zap_string_t, ptr) == 0,
               "String ABI: ptr offset mismatch");
_Static_assert(offsetof(zap_string_t, len) == sizeof(const char *),
               "String ABI: value len offset mismatch");

char *string_concat_ptrlen(const char *a, long a_len, const char *b,
                           long b_len) {
  if (a_len < 0 || b_len < 0 || (size_t)a_len > SIZE_MAX - (size_t)b_len) {
    return NULL;
  }
  size_t total = (size_t)a_len + (size_t)b_len;
  char *out = zap_string_alloc_owned(total);
  if (!out)
    return NULL;
  if (a_len > 0)
    memcpy(out, a, (size_t)a_len);
  if (b_len > 0)
    memcpy(out + a_len, b, (size_t)b_len);
  out[total] = '\0';
  return out;
}

static zap_string_header_t *zap_string_header_from_ptr(const char *ptr) {
  if (!ptr) {
    return NULL;
  }
  return (zap_string_header_t *)((char *)ptr - sizeof(zap_string_header_t));
}

char *zap_string_alloc_owned(size_t len) {
  if (len > SIZE_MAX - sizeof(zap_string_header_t) - 1) {
    zap_runtime_out_of_memory();
  }
  zap_string_header_t *header = (zap_string_header_t *)zap_runtime_alloc(
      sizeof(zap_string_header_t) + len + 1);
  header->refs = 1;
  header->len = (int64_t)len;
  char *ptr = (char *)(header + 1);
  ptr[len] = '\0';
  return ptr;
}

static void zap_string_retain_ptr(const char *ptr) {
  zap_string_header_t *header = zap_string_header_from_ptr(ptr);
  if (!header || header->refs == ZAP_STRING_IMMORTAL_REFCOUNT) {
    return;
  }
  header->refs += 1;
}

void zap_string_release_ptr(const char *ptr) {
  zap_string_header_t *header = zap_string_header_from_ptr(ptr);
  if (!header || header->refs == ZAP_STRING_IMMORTAL_REFCOUNT) {
    return;
  }
  header->refs -= 1;
  if (header->refs <= 0) {
    free(header);
  }
}

char *zap_string_to_cstr(zap_string_t s) {
  size_t len = s.len > 0 ? (size_t)s.len : 0;
  char *out = (char *)malloc(len + 1);
  if (!out)
    return NULL;
  if (len > 0 && s.ptr)
    memcpy(out, s.ptr, len);
  out[len] = '\0';
  return out;
}

zap_string_t zap_string_from_cstr(const char *cstr) {
  if (!cstr) {
    return (zap_string_t){.ptr = NULL, .len = 0};
  }
  size_t len = strlen(cstr);
  if (len == 0) {
    return (zap_string_t){.ptr = NULL, .len = 0};
  }
  char *out = zap_string_alloc_owned(len);
  if (!out) {
    return (zap_string_t){.ptr = NULL, .len = 0};
  }
  if (len > 0) {
    memcpy(out, cstr, len);
  }
  out[len] = '\0';
  return (zap_string_t){.ptr = out, .len = (long)len};
}

zap_string_t zap_string_from_ptrlen(const char *ptr, long len) {
  if (!ptr || len <= 0) {
    return (zap_string_t){.ptr = NULL, .len = 0};
  }

  char *out = zap_string_alloc_owned((size_t)len);
  if (!out) {
    return (zap_string_t){.ptr = NULL, .len = 0};
  }

  memcpy(out, ptr, (size_t)len);
  out[len] = '\0';
  return (zap_string_t){.ptr = out, .len = len};
}

zap_string_t zap_string_retain(zap_string_t s) {
  zap_string_retain_ptr(s.ptr);
  return s;
}

void zap_string_release(zap_string_t s) { zap_string_release_ptr(s.ptr); }

static zap_string_t zap_string_from_format(const char *format, ...) {
  va_list args;
  va_start(args, format);
  va_list args_copy;
  va_copy(args_copy, args);
  int needed = vsnprintf(NULL, 0, format, args_copy);
  va_end(args_copy);

  if (needed < 0) {
    va_end(args);
    return (zap_string_t){.ptr = NULL, .len = 0};
  }

  char *buffer = (char *)malloc((size_t)needed + 1);
  if (!buffer) {
    va_end(args);
    return (zap_string_t){.ptr = NULL, .len = 0};
  }

  vsnprintf(buffer, (size_t)needed + 1, format, args);
  va_end(args);
  zap_string_t result = zap_string_from_cstr(buffer);
  free(buffer);
  return result;
}

zap_string_t zap_to_string_i64(int64_t value) {
  return zap_string_from_format("%lld", (long long)value);
}

zap_string_t zap_to_string_u64(uint64_t value) {
  return zap_string_from_format("%llu", (unsigned long long)value);
}

zap_string_t zap_to_string_f64(double value) {
  return zap_string_from_format("%g", value);
}

long zap_to_int_from_char(char value) { return (unsigned char)value; }

char zap_to_char_from_int(long value) { return (char)value; }

long len(zap_string_t s) { return s.len; }

char at(zap_string_t s, long i) {
  if (!s.ptr || i < 0 || i >= s.len) {
    return '\0';
  }
  return s.ptr[i];
}

zap_string_t slice(zap_string_t s, long start, long length) {
  if (!s.ptr || s.len <= 0 || length <= 0 || start >= s.len) {
    return (zap_string_t){.ptr = NULL, .len = 0};
  }

  if (start < 0) {
    start = 0;
  }

  long available = s.len - start;
  if (available < 0) {
    available = 0;
  }
  if (length > available) {
    length = available;
  }

  char *out = zap_string_alloc_owned((size_t)length);
  if (!out) {
    return (zap_string_t){.ptr = NULL, .len = 0};
  }

  if (length > 0) {
    memcpy(out, s.ptr + start, (size_t)length);
  }
  out[length] = '\0';
  return (zap_string_t){.ptr = out, .len = length};
}

bool eq(zap_string_t a, zap_string_t b) {
  if (a.len != b.len) {
    return 0;
  }

  if (a.len == 0) {
    return 1;
  }

  if (!a.ptr || !b.ptr) {
    return 0;
  }

  return memcmp(a.ptr, b.ptr, (size_t)a.len) == 0;
}
