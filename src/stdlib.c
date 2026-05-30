#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "runtime/arc_layout.h"

typedef struct zap_arc_metadata_t {
  uint32_t strong_field_count;
  const uint32_t *strong_field_offsets;
} zap_arc_metadata_t;

typedef struct zap_arc_header_t {
  int64_t strong_count;
  int64_t weak_count;
  uint8_t alive;
  uint8_t gc_mark;
  void (*release_fn)(void *);
  void (*destroy_fn)(void *);
  const zap_arc_metadata_t *metadata;
  void **vtable;
} zap_arc_header_t;

_Static_assert(ZAP_ARC_STRONG_COUNT_INDEX == 0,
               "ARC ABI: strong_count index mismatch");
_Static_assert(ZAP_ARC_WEAK_COUNT_INDEX == 1,
               "ARC ABI: weak_count index mismatch");
_Static_assert(ZAP_ARC_ALIVE_INDEX == 2, "ARC ABI: alive index mismatch");
_Static_assert(ZAP_ARC_GC_MARK_INDEX == 3, "ARC ABI: gc_mark index mismatch");
_Static_assert(ZAP_ARC_RELEASE_FN_INDEX == 4,
               "ARC ABI: release_fn index mismatch");
_Static_assert(ZAP_ARC_DESTROY_FN_INDEX == 5,
               "ARC ABI: destroy_fn index mismatch");
_Static_assert(ZAP_ARC_METADATA_INDEX == 6, "ARC ABI: metadata index mismatch");
_Static_assert(ZAP_ARC_VTABLE_INDEX == 7, "ARC ABI: vtable index mismatch");
_Static_assert(ZAP_ARC_FIELD_START_INDEX == 8,
               "ARC ABI: field start index mismatch");

_Static_assert(offsetof(zap_arc_header_t, strong_count) == 0,
               "ARC ABI: strong_count offset mismatch");
_Static_assert(offsetof(zap_arc_header_t, weak_count) >
                   offsetof(zap_arc_header_t, strong_count),
               "ARC ABI: weak_count must be after strong_count");
_Static_assert(offsetof(zap_arc_header_t, alive) >
                   offsetof(zap_arc_header_t, weak_count),
               "ARC ABI: alive must be after weak_count");
_Static_assert(offsetof(zap_arc_header_t, gc_mark) >
                   offsetof(zap_arc_header_t, alive),
               "ARC ABI: gc_mark must be after alive");
_Static_assert(offsetof(zap_arc_header_t, release_fn) >
                   offsetof(zap_arc_header_t, gc_mark),
               "ARC ABI: release_fn must be after gc_mark");
_Static_assert(offsetof(zap_arc_header_t, destroy_fn) >
                   offsetof(zap_arc_header_t, release_fn),
               "ARC ABI: destroy_fn must be after release_fn");
_Static_assert(offsetof(zap_arc_header_t, metadata) >
                   offsetof(zap_arc_header_t, destroy_fn),
               "ARC ABI: metadata must be after destroy_fn");
_Static_assert(offsetof(zap_arc_header_t, vtable) >
                   offsetof(zap_arc_header_t, metadata),
               "ARC ABI: vtable must be after metadata");

static void **zap_arc_objects = NULL;
static size_t zap_arc_object_count = 0;
static size_t zap_arc_object_capacity = 0;
static int zap_arc_collecting = 0;
static int zap_net_last_error = 0;

static int zap_arc_find_object_index(void *object) {
  for (size_t i = 0; i < zap_arc_object_count; ++i) {
    if (zap_arc_objects[i] == object) {
      return (int)i;
    }
  }
  return -1;
}

void zap_arc_register(void *object) {
  if (!object || zap_arc_find_object_index(object) >= 0) {
    return;
  }

  if (zap_arc_object_count == zap_arc_object_capacity) {
    size_t next_capacity =
        zap_arc_object_capacity == 0 ? 16 : zap_arc_object_capacity * 2;
    void **next =
        (void **)realloc(zap_arc_objects, next_capacity * sizeof(void *));
    if (!next) {
      return;
    }
    zap_arc_objects = next;
    zap_arc_object_capacity = next_capacity;
  }

  zap_arc_objects[zap_arc_object_count++] = object;
}

void zap_arc_unregister(void *object) {
  int index = zap_arc_find_object_index(object);
  if (index < 0) {
    return;
  }

  size_t last = zap_arc_object_count - 1;
  zap_arc_objects[(size_t)index] = zap_arc_objects[last];
  zap_arc_object_count = last;
}

static void zap_arc_collect_edge(void *child, void *ctx) {
  int *incoming = (int *)ctx;
  int index = zap_arc_find_object_index(child);
  if (index >= 0) {
    incoming[index] += 1;
  }
}

static void zap_arc_mark_reachable(void *object, uint8_t *reachable) {
  int index = zap_arc_find_object_index(object);
  if (index < 0 || reachable[index]) {
    return;
  }

  reachable[index] = 1;
  zap_arc_header_t *header = (zap_arc_header_t *)object;
  if (!header->alive || !header->metadata) {
    return;
  }

  for (uint32_t i = 0; i < header->metadata->strong_field_count; ++i) {
    uint32_t offset = header->metadata->strong_field_offsets[i];
    void **field_addr = (void **)((char *)object + offset);
    void *child = *field_addr;
    if (child) {
      zap_arc_mark_reachable(child, reachable);
    }
  }
}

void zap_arc_cycle_collect(void) {
  if (zap_arc_collecting || zap_arc_object_count == 0) {
    return;
  }

  zap_arc_collecting = 1;

  int *incoming = (int *)calloc(zap_arc_object_count, sizeof(int));
  uint8_t *reachable = (uint8_t *)calloc(zap_arc_object_count, sizeof(uint8_t));
  if (!incoming || !reachable) {
    free(incoming);
    free(reachable);
    zap_arc_collecting = 0;
    return;
  }

  for (size_t i = 0; i < zap_arc_object_count; ++i) {
    zap_arc_header_t *header = (zap_arc_header_t *)zap_arc_objects[i];
    if (!header->alive || !header->metadata) {
      continue;
    }
    for (uint32_t j = 0; j < header->metadata->strong_field_count; ++j) {
      uint32_t offset = header->metadata->strong_field_offsets[j];
      void **field_addr = (void **)((char *)zap_arc_objects[i] + offset);
      void *child = *field_addr;
      if (child) {
        zap_arc_collect_edge(child, incoming);
      }
    }
  }

  for (size_t i = 0; i < zap_arc_object_count; ++i) {
    zap_arc_header_t *header = (zap_arc_header_t *)zap_arc_objects[i];
    if (!header->alive) {
      continue;
    }
    if (header->strong_count > incoming[i]) {
      zap_arc_mark_reachable(zap_arc_objects[i], reachable);
    }
  }

  for (size_t i = 0; i < zap_arc_object_count; ++i) {
    zap_arc_header_t *header = (zap_arc_header_t *)zap_arc_objects[i];
    if (!header->alive || reachable[i]) {
      continue;
    }
    header->gc_mark = 1;
  }

  for (size_t i = 0; i < zap_arc_object_count; ++i) {
    zap_arc_header_t *header = (zap_arc_header_t *)zap_arc_objects[i];
    if (!header->alive || !header->gc_mark) {
      continue;
    }
    header->strong_count = 1;
  }

  size_t garbage_count = 0;
  for (size_t i = 0; i < zap_arc_object_count; ++i) {
    zap_arc_header_t *header = (zap_arc_header_t *)zap_arc_objects[i];
    if (header->alive && header->gc_mark) {
      garbage_count += 1;
    }
  }

  void **garbage = garbage_count > 0
                       ? (void **)malloc(garbage_count * sizeof(void *))
                       : NULL;
  if (garbage_count > 0 && !garbage) {
    free(incoming);
    free(reachable);
    zap_arc_collecting = 0;
    return;
  }

  size_t cursor = 0;
  for (size_t i = 0; i < zap_arc_object_count; ++i) {
    zap_arc_header_t *header = (zap_arc_header_t *)zap_arc_objects[i];
    if (header->alive && header->gc_mark) {
      garbage[cursor++] = zap_arc_objects[i];
    }
  }

  for (size_t i = 0; i < garbage_count; ++i) {
    zap_arc_header_t *header = (zap_arc_header_t *)garbage[i];
    if (!header->alive || !header->release_fn) {
      continue;
    }
    header->release_fn(garbage[i]);
  }

  free(garbage);
  free(incoming);
  free(reachable);
  zap_arc_collecting = 0;
}

void printInt(long v) { printf("%ld\n", v); }

void printFloat(float v) { printf("%f\n", v); }

void printFloat64(double v) { printf("%f\n", v); }

long zap_sum_variadic(long count, ...) {
  va_list args;
  va_start(args, count);
  long sum = 0;
  for (long i = 0; i < count; ++i) {
    sum += va_arg(args, long);
  }
  va_end(args);
  return sum;
}

void printBool(_Bool v) { printf("%s\n", v ? "true" : "false"); }

void printChar(char v) {
  putchar(v);
  putchar('\n');
}

void printStringPtrLen(const char *ptr, long len) {
  if (!ptr || len <= 0) {
    printf("\n");
    return;
  }
  fwrite(ptr, 1, (size_t)len, stdout);
  printf("\n");
}

static void zap_string_register_owned_ptr(const char *ptr);
static char *zap_string_alloc_owned(size_t len);

char *string_concat_ptrlen(const char *a, long a_len, const char *b,
                           long b_len) {
  long total = a_len + b_len;
  char *out = zap_string_alloc_owned((size_t)total);
  if (!out)
    return NULL;
  if (a_len > 0)
    memcpy(out, a, (size_t)a_len);
  if (b_len > 0)
    memcpy(out + a_len, b, (size_t)b_len);
  out[total] = '\0';
  return out;
}

typedef struct {
  const char *ptr;
  long len;
} zap_string_t;

typedef struct zap_string_owner_entry_t {
  const char *ptr;
  struct zap_string_owner_entry_t *next;
} zap_string_owner_entry_t;

static zap_string_owner_entry_t *zap_string_owners = NULL;
static const uint64_t ZAP_STRING_HEADER_MAGIC = 0x5A41505354524E47ULL;

typedef struct {
  uint64_t magic;
  int64_t refs;
  int64_t len;
} zap_string_header_t;

static zap_string_owner_entry_t *zap_string_find_owner(const char *ptr) {
  zap_string_owner_entry_t *entry = zap_string_owners;
  while (entry) {
    if (entry->ptr == ptr) {
      return entry;
    }
    entry = entry->next;
  }
  return NULL;
}

static void zap_string_register_owned_ptr(const char *ptr) {
  if (!ptr) {
    return;
  }
  zap_string_owner_entry_t *existing = zap_string_find_owner(ptr);
  if (existing) {
    return;
  }
  zap_string_owner_entry_t *entry =
      (zap_string_owner_entry_t *)malloc(sizeof(zap_string_owner_entry_t));
  if (!entry) {
    return;
  }
  entry->ptr = ptr;
  entry->next = zap_string_owners;
  zap_string_owners = entry;
}

static void zap_string_unregister_owned_ptr(const char *ptr) {
  zap_string_owner_entry_t *prev = NULL;
  zap_string_owner_entry_t *entry = zap_string_owners;
  while (entry) {
    if (entry->ptr == ptr) {
      if (prev) {
        prev->next = entry->next;
      } else {
        zap_string_owners = entry->next;
      }
      free(entry);
      return;
    }
    prev = entry;
    entry = entry->next;
  }
}

static zap_string_header_t *zap_string_header_from_ptr(const char *ptr) {
  if (!ptr || !zap_string_find_owner(ptr)) {
    return NULL;
  }
  return (zap_string_header_t *)((char *)ptr - sizeof(zap_string_header_t));
}

static char *zap_string_alloc_owned(size_t len) {
  zap_string_header_t *header = (zap_string_header_t *)malloc(
      sizeof(zap_string_header_t) + len + 1);
  if (!header) {
    return NULL;
  }
  header->magic = ZAP_STRING_HEADER_MAGIC;
  header->refs = 1;
  header->len = (int64_t)len;
  char *ptr = (char *)(header + 1);
  ptr[len] = '\0';
  zap_string_register_owned_ptr(ptr);
  return ptr;
}

static void zap_string_retain_ptr(const char *ptr) {
  zap_string_header_t *header = zap_string_header_from_ptr(ptr);
  if (!header || header->magic != ZAP_STRING_HEADER_MAGIC) {
    return;
  }
  header->refs += 1;
}

static void zap_string_release_ptr(const char *ptr) {
  zap_string_header_t *header = zap_string_header_from_ptr(ptr);
  if (!header || header->magic != ZAP_STRING_HEADER_MAGIC) {
    return;
  }
  header->refs -= 1;
  if (header->refs <= 0) {
    zap_string_unregister_owned_ptr(ptr);
    free(header);
  }
}

void print(zap_string_t s) {
  if (!s.ptr || s.len <= 0) {
    return;
  }
  fwrite(s.ptr, 1, (size_t)s.len, stdout);
}

static char *zap_string_to_cstr(zap_string_t s) {
  size_t len = s.len > 0 ? (size_t)s.len : 0;
  char *out = (char *)malloc(len + 1);
  if (!out)
    return NULL;
  if (len > 0 && s.ptr)
    memcpy(out, s.ptr, len);
  out[len] = '\0';
  return out;
}

static zap_string_t zap_string_from_owned(char *owned) {
  if (!owned) {
    return (zap_string_t){.ptr = NULL, .len = 0};
  }
  if (!zap_string_find_owner(owned)) {
    zap_string_register_owned_ptr(owned);
  }
  return (zap_string_t){.ptr = owned, .len = (long)strlen(owned)};
}

zap_string_t zap_string_from_cstr(const char *cstr) {
  if (!cstr) {
    return (zap_string_t){.ptr = "", .len = 0};
  }
  size_t len = strlen(cstr);
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
    return (zap_string_t){.ptr = "", .len = 0};
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

static long zap_process_argc = 0;
static char **zap_process_argv = NULL;

void __zap_process_set_args(int argc, char **argv) {
  zap_process_argc = argc;
  zap_process_argv = argv;
}

void println(zap_string_t s) { printStringPtrLen(s.ptr, s.len); }

long zap_printf(zap_string_t format, ...) {
  char *fmt = zap_string_to_cstr(format);
  if (!fmt)
    return -1;

  va_list args;
  va_start(args, format);
  long written = vprintf(fmt, args);
  va_end(args);

  free(fmt);
  return written;
}

long zap_printfln(zap_string_t format, ...) {
  char *fmt = zap_string_to_cstr(format);
  if (!fmt)
    return -1;

  va_list args;
  va_start(args, format);
  long written = vprintf(fmt, args);
  va_end(args);

  free(fmt);

  if (written < 0)
    return written;
  if (printf("\n") < 0)
    return -1;
  return written + 1;
}

void eprintln(zap_string_t s) {
  if (!s.ptr || s.len <= 0) {
    fputc('\n', stderr);
    return;
  }
  fwrite(s.ptr, 1, (size_t)s.len, stderr);
  fputc('\n', stderr);
}

void println_cstr(const char *s) {
  if (!s) {
    printf("\n");
    return;
  }
  puts(s);
}

zap_string_t getLn() {
  char *line = NULL;
  size_t len = 0;
  size_t read = getline(&line, &len, stdin);
  if (read == -1) {
    free(line);
    return (zap_string_t){.ptr = NULL, .len = 0};
  }
  // Remove newline if present
  if (read > 0 && line[read - 1] == '\n') {
    line[--read] = '\0';
  }
  char *owned = zap_string_alloc_owned(read);
  if (!owned) {
    free(line);
    return (zap_string_t){.ptr = NULL, .len = 0};
  }
  if (read > 0) {
    memcpy(owned, line, read);
  }
  owned[read] = '\0';
  free(line);
  zap_string_t result = {.ptr = owned, .len = read};
  return result;
}

long argc() { return zap_process_argc; }

zap_string_t argv(long i) {
  if (i < 0 || i >= zap_process_argc || !zap_process_argv ||
      !zap_process_argv[i]) {
    return (zap_string_t){.ptr = "", .len = 0};
  }

  const char *arg = zap_process_argv[i];
  return (zap_string_t){.ptr = arg, .len = (long)strlen(arg)};
}

long len(zap_string_t s) { return s.len; }

char at(zap_string_t s, long i) {
  if (!s.ptr || i < 0 || i >= s.len) {
    return '\0';
  }
  return s.ptr[i];
}

zap_string_t slice(zap_string_t s, long start, long length) {
  if (!s.ptr || s.len <= 0 || length <= 0 || start >= s.len) {
    return (zap_string_t){.ptr = "", .len = 0};
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

_Bool eq(zap_string_t a, zap_string_t b) {
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

long exec(zap_string_t cmd) {
  if (!cmd.ptr) {
    return -1;
  }

  char *buffer = (char *)malloc((size_t)cmd.len + 1);
  if (!buffer) {
    return -1;
  }

  memcpy(buffer, cmd.ptr, (size_t)cmd.len);
  buffer[cmd.len] = '\0';

  int result = system(buffer);
  free(buffer);

  if (result == -1) {
    return -1;
  }

  if (WIFEXITED(result)) {
    return WEXITSTATUS(result);
  }

  return result;
}

zap_string_t cwd() {
  char *dir = getcwd(NULL, 0);
  if (!dir) {
    return (zap_string_t){.ptr = "", .len = 0};
  }
  size_t dir_len = strlen(dir);
  char *owned = zap_string_alloc_owned(dir_len);
  if (!owned) {
    free(dir);
    return (zap_string_t){.ptr = NULL, .len = 0};
  }
  if (dir_len > 0) {
    memcpy(owned, dir, dir_len);
  }
  owned[dir_len] = '\0';
  free(dir);
  return (zap_string_t){.ptr = owned, .len = (long)dir_len};
}

static int zap_stat_path(zap_string_t path, struct stat *st) {
  if (!path.ptr) {
    return -1;
  }

  char *buffer = (char *)malloc((size_t)path.len + 1);
  if (!buffer) {
    return -1;
  }

  memcpy(buffer, path.ptr, (size_t)path.len);
  buffer[path.len] = '\0';

  int result = stat(buffer, st);
  free(buffer);
  return result;
}

static char *zap_copy_path(zap_string_t path) {
  if (!path.ptr) {
    return NULL;
  }

  char *buffer = (char *)malloc((size_t)path.len + 1);
  if (!buffer) {
    return NULL;
  }

  memcpy(buffer, path.ptr, (size_t)path.len);
  buffer[path.len] = '\0';
  return buffer;
}

static int zap_net_bind_addrinfo(const char *host, long port,
                                 int socktype, int flags,
                                 struct addrinfo **out) {
  if (!out) {
    zap_net_last_error = EINVAL;
    return -1;
  }

  char port_buf[32];
  snprintf(port_buf, sizeof(port_buf), "%ld", port);

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = socktype;
  hints.ai_flags = flags;

  const char *node = host;
  if (host && (strcmp(host, "") == 0 || strcmp(host, "*") == 0)) {
    node = NULL;
  }

  int rc = getaddrinfo(node, port_buf, &hints, out);
  if (rc != 0) {
    if (rc == EAI_SYSTEM) {
      zap_net_last_error = errno;
    } else {
      zap_net_last_error = EINVAL;
    }
    return -1;
  }

  zap_net_last_error = 0;
  return 0;
}

_Bool exists(zap_string_t path) {
  struct stat st;
  return zap_stat_path(path, &st) == 0;
}

_Bool isFile(zap_string_t path) {
  struct stat st;
  if (zap_stat_path(path, &st) != 0) {
    return 0;
  }

  return S_ISREG(st.st_mode);
}

_Bool isDir(zap_string_t path) {
  struct stat st;
  if (zap_stat_path(path, &st) != 0) {
    return 0;
  }

  return S_ISDIR(st.st_mode);
}

long zap_fs_mkdir(zap_string_t path) {
  char *buffer = zap_copy_path(path);
  if (!buffer) {
    return ENOMEM;
  }

  if (mkdir(buffer, 0777) == 0) {
    free(buffer);
    return 0;
  }

  int err = errno;
  free(buffer);
  return err;
}

long zap_fs_remove(zap_string_t path) {
  char *buffer = zap_copy_path(path);
  if (!buffer) {
    return ENOMEM;
  }

  if (remove(buffer) == 0) {
    free(buffer);
    return 0;
  }

  int err = errno;
  free(buffer);
  return err;
}

long zap_fs_rename(zap_string_t from, zap_string_t to) {
  char *from_buffer = zap_copy_path(from);
  if (!from_buffer) {
    return ENOMEM;
  }

  char *to_buffer = zap_copy_path(to);
  if (!to_buffer) {
    free(from_buffer);
    return ENOMEM;
  }

  if (rename(from_buffer, to_buffer) == 0) {
    free(from_buffer);
    free(to_buffer);
    return 0;
  }

  int err = errno;
  free(from_buffer);
  free(to_buffer);
  return err;
}

zap_string_t readFile(zap_string_t path) {
  char *buffer = zap_copy_path(path);
  if (!buffer) {
    return (zap_string_t){.ptr = "", .len = 0};
  }

  FILE *file = fopen(buffer, "rb");
  free(buffer);
  if (!file) {
    return (zap_string_t){.ptr = "", .len = 0};
  }

  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return (zap_string_t){.ptr = "", .len = 0};
  }

  long size = ftell(file);
  if (size < 0) {
    fclose(file);
    return (zap_string_t){.ptr = "", .len = 0};
  }

  if (fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return (zap_string_t){.ptr = "", .len = 0};
  }

  char *content = zap_string_alloc_owned((size_t)size);
  if (!content) {
    fclose(file);
    return (zap_string_t){.ptr = "", .len = 0};
  }

  size_t read = fread(content, 1, (size_t)size, file);
  fclose(file);
  if (read != (size_t)size) {
    zap_string_release_ptr(content);
    return (zap_string_t){.ptr = "", .len = 0};
  }

  content[size] = '\0';
  return (zap_string_t){.ptr = content, .len = size};
}

long writeFile(zap_string_t path, zap_string_t content) {
  char *buffer = zap_copy_path(path);
  if (!buffer) {
    return ENOMEM;
  }

  FILE *file = fopen(buffer, "wb");
  free(buffer);
  if (!file) {
    return errno;
  }

  size_t written = fwrite(content.ptr, 1, (size_t)content.len, file);
  if (fclose(file) != 0) {
    return errno;
  }

  if (written != (size_t)content.len) {
    return EIO;
  }

  return 0;
}

static zap_string_t zap_copy_string_range(const char *start, size_t len) {
  char *out = zap_string_alloc_owned(len);
  if (!out) {
    return (zap_string_t){.ptr = "", .len = 0};
  }

  if (len > 0) {
    memcpy(out, start, len);
  }
  out[len] = '\0';
  return (zap_string_t){.ptr = out, .len = (long)len};
}

double zapMathSqrt(double x) { return sqrt(x); }

double zapMathFloor(double x) { return floor(x); }

double zapMathCeil(double x) { return ceil(x); }

long netConnect(zap_string_t host, long port) {
  if (!host.ptr || port <= 0 || port > 65535) {
    zap_net_last_error = EINVAL;
    return -1;
  }

  char *host_buf = zap_copy_path(host);
  if (!host_buf) {
    zap_net_last_error = ENOMEM;
    return -1;
  }

  struct addrinfo *res = NULL;
  if (zap_net_bind_addrinfo(host_buf, port, SOCK_STREAM, 0, &res) != 0) {
    free(host_buf);
    return -1;
  }

  long out_fd = -1;
  int last_err = ECONNREFUSED;
  for (struct addrinfo *it = res; it; it = it->ai_next) {
    int fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
    if (fd < 0) {
      last_err = errno;
      continue;
    }

    if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) {
      out_fd = fd;
      last_err = 0;
      break;
    }

    last_err = errno;
    close(fd);
  }

  freeaddrinfo(res);
  free(host_buf);

  zap_net_last_error = last_err;
  return out_fd;
}

long netListen(zap_string_t host, long port) {
  if (port <= 0 || port > 65535) {
    zap_net_last_error = EINVAL;
    return -1;
  }

  char *host_buf = NULL;
  if (host.ptr) {
    host_buf = zap_copy_path(host);
    if (!host_buf) {
      zap_net_last_error = ENOMEM;
      return -1;
    }
  }

  struct addrinfo *res = NULL;
  if (zap_net_bind_addrinfo(host_buf ? host_buf : "", port, SOCK_STREAM,
                            AI_PASSIVE, &res) != 0) {
    free(host_buf);
    return -1;
  }

  long out_fd = -1;
  int last_err = EADDRNOTAVAIL;
  for (struct addrinfo *it = res; it; it = it->ai_next) {
    int fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
    if (fd < 0) {
      last_err = errno;
      continue;
    }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    if (bind(fd, it->ai_addr, it->ai_addrlen) != 0) {
      last_err = errno;
      close(fd);
      continue;
    }

    if (listen(fd, 128) != 0) {
      last_err = errno;
      close(fd);
      continue;
    }

    out_fd = fd;
    last_err = 0;
    break;
  }

  freeaddrinfo(res);
  free(host_buf);

  zap_net_last_error = last_err;
  return out_fd;
}

long netAccept(long listenerFd) {
  if (listenerFd < 0) {
    zap_net_last_error = EINVAL;
    return -1;
  }

  int fd = accept((int)listenerFd, NULL, NULL);
  if (fd < 0) {
    zap_net_last_error = errno;
    return -1;
  }

  zap_net_last_error = 0;
  return fd;
}

long netClose(long fd) {
  if (fd < 0) {
    zap_net_last_error = EINVAL;
    return EINVAL;
  }

  if (close((int)fd) != 0) {
    zap_net_last_error = errno;
    return errno;
  }

  zap_net_last_error = 0;
  return 0;
}

long netSend(long fd, zap_string_t data) {
  if (fd < 0 || !data.ptr) {
    zap_net_last_error = EINVAL;
    return -1;
  }

  size_t total = 0;
  size_t target = data.len > 0 ? (size_t)data.len : 0;

  while (total < target) {
    ssize_t n = send((int)fd, data.ptr + total, target - total, 0);
    if (n < 0) {
      zap_net_last_error = errno;
      return -1;
    }
    if (n == 0) {
      break;
    }
    total += (size_t)n;
  }

  zap_net_last_error = 0;
  return (long)total;
}

zap_string_t netRecv(long fd, long maxLen) {
  if (fd < 0 || maxLen <= 0) {
    zap_net_last_error = EINVAL;
    return (zap_string_t){.ptr = "", .len = 0};
  }

  size_t cap = (size_t)maxLen;
  char *buf = zap_string_alloc_owned(cap);
  if (!buf) {
    zap_net_last_error = ENOMEM;
    return (zap_string_t){.ptr = "", .len = 0};
  }

  ssize_t n;
  do {
    n = recv((int)fd, buf, cap, 0);
  } while (n < 0 && errno == EINTR);
  if (n < 0) {
    zap_net_last_error = errno;
    zap_string_release_ptr(buf);
    return (zap_string_t){.ptr = "", .len = 0};
  }

  buf[n] = '\0';
  zap_net_last_error = 0;
  return (zap_string_t){.ptr = buf, .len = (long)n};
}

zap_string_t netResolve(zap_string_t host) {
  if (!host.ptr || host.len == 0) {
    zap_net_last_error = EINVAL;
    return (zap_string_t){.ptr = "", .len = 0};
  }

  char *host_buf = zap_copy_path(host);
  if (!host_buf) {
    zap_net_last_error = ENOMEM;
    return (zap_string_t){.ptr = "", .len = 0};
  }

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo *res = NULL;
  int rc = getaddrinfo(host_buf, NULL, &hints, &res);
  free(host_buf);
  if (rc != 0) {
    if (rc == EAI_SYSTEM) {
      zap_net_last_error = errno;
    } else {
      zap_net_last_error = EINVAL;
    }
    return (zap_string_t){.ptr = "", .len = 0};
  }

  char ipbuf[INET6_ADDRSTRLEN];
  memset(ipbuf, 0, sizeof(ipbuf));

  for (struct addrinfo *it = res; it; it = it->ai_next) {
    void *addr_ptr = NULL;
    if (it->ai_family == AF_INET) {
      struct sockaddr_in *sa = (struct sockaddr_in *)it->ai_addr;
      addr_ptr = &(sa->sin_addr);
    } else if (it->ai_family == AF_INET6) {
      struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)it->ai_addr;
      addr_ptr = &(sa6->sin6_addr);
    }

    if (addr_ptr &&
        inet_ntop(it->ai_family, addr_ptr, ipbuf, sizeof(ipbuf)) != NULL) {
      break;
    }
  }

  freeaddrinfo(res);

  if (ipbuf[0] == '\0') {
    zap_net_last_error = EADDRNOTAVAIL;
    return (zap_string_t){.ptr = "", .len = 0};
  }

  zap_net_last_error = 0;
  return zap_copy_string_range(ipbuf, strlen(ipbuf));
}

long netLastError() { return zap_net_last_error; }
