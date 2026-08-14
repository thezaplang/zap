#include "runtime/arc_layout.h"
#include "runtime/string_layout.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <netdb.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

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

static int zap_net_last_error = 0;
static long zap_tls_last_error_code = 0;
static long zap_fs_last_error_code = 0;

#if defined(ZAP_RUNTIME_INSTRUMENTATION)
static zap_runtime_ownership_counters_t zap_runtime_ownership_counters;
static int zap_runtime_fail_arc_scratch_allocation = 0;

void zap_runtime_ownership_reset_counters(void) {
  memset(&zap_runtime_ownership_counters, 0,
         sizeof(zap_runtime_ownership_counters));
}

void zap_runtime_ownership_snapshot_counters(
    zap_runtime_ownership_counters_t *out) {
  if (out) {
    *out = zap_runtime_ownership_counters;
  }
}

void zap_runtime_ownership_note_strong_retain(void) {
  ++zap_runtime_ownership_counters.strong_retain_calls;
}

void zap_runtime_ownership_note_strong_release(void) {
  ++zap_runtime_ownership_counters.strong_release_calls;
}

void zap_runtime_ownership_note_copy(void) {
  ++zap_runtime_ownership_counters.copy_operations;
}

void zap_runtime_ownership_note_drop(void) {
  ++zap_runtime_ownership_counters.drop_operations;
}

void zap_runtime_ownership_note_destroy(void) {
  ++zap_runtime_ownership_counters.destroy_calls;
}

uint64_t zap_runtime_ownership_copy_operations(void) {
  return zap_runtime_ownership_counters.copy_operations;
}

uint64_t zap_runtime_ownership_drop_operations(void) {
  return zap_runtime_ownership_counters.drop_operations;
}

uint64_t zap_runtime_ownership_destroy_calls(void) {
  return zap_runtime_ownership_counters.destroy_calls;
}

void zap_runtime_test_fail_next_arc_scratch_allocation(void) {
  zap_runtime_fail_arc_scratch_allocation = 1;
}
#endif

static void zap_runtime_out_of_memory(void) {
  fputs("zap runtime error: out of memory\n", stderr);
  abort();
}

static void zap_arc_graph_limit_exceeded(void) {
  fputs("zap runtime error: ARC collector graph exceeds uint32 index limit\n",
        stderr);
  abort();
}

void *zap_runtime_alloc(size_t size) {
  void *allocation = malloc(size);
  if (!allocation) {
    zap_runtime_out_of_memory();
  }
#if defined(ZAP_RUNTIME_INSTRUMENTATION)
  ++zap_runtime_ownership_counters.allocations;
#endif
  return allocation;
}

static void *zap_runtime_realloc_array(void *allocation, size_t count,
                                       size_t element_size) {
  if (count != 0 && element_size > SIZE_MAX / count) {
    zap_runtime_out_of_memory();
  }
  void *resized = realloc(allocation, count * element_size);
  if (!resized) {
    zap_runtime_out_of_memory();
  }
  return resized;
}

static void *zap_runtime_calloc_array(size_t count, size_t element_size) {
  if (count != 0 && element_size > SIZE_MAX / count) {
    zap_runtime_out_of_memory();
  }
  void *allocation = calloc(count, element_size);
  if (!allocation) {
    zap_runtime_out_of_memory();
  }
  return allocation;
}

static size_t zap_runtime_next_capacity(size_t capacity,
                                        size_t initial_capacity) {
  if (capacity == 0) {
    return initial_capacity;
  }
  if (capacity > SIZE_MAX / 2) {
    zap_runtime_out_of_memory();
  }
  return capacity * 2;
}

void zap_arc_strong_refcount_overflow(void) {
  fputs("zap runtime error: strong ARC reference count overflow\n", stderr);
  abort();
}

void zap_arc_weak_refcount_overflow(void) {
  fputs("zap runtime error: weak ARC reference count overflow\n", stderr);
  abort();
}

void zap_arc_strong_refcount_underflow(void) {
  fputs("zap runtime error: strong ARC reference count underflow\n", stderr);
  abort();
}

void zap_arc_weak_refcount_underflow(void) {
  fputs("zap runtime error: weak ARC reference count underflow\n", stderr);
  abort();
}

void zap_arc_retain_dead_object(void) {
  fputs("zap runtime error: cannot retain a dead ARC object\n", stderr);
  abort();
}

typedef struct {
  void **keys;
  uint32_t *vals;
  size_t cap;
  size_t len;
} zap_arc_ptrmap_t;

struct zap_arc_runtime_context_t {
  void **roots;
  size_t root_count;
  size_t root_capacity;
  int collecting;
  int collection_pending;
  void **snap;
  size_t snap_cap;
  void **worklist;
  size_t worklist_cap;
  int *incoming;
  uint8_t *reachable;
  uint32_t *stack;
  size_t scratch_cap;
  zap_arc_ptrmap_t map;
};

static zap_arc_runtime_context_t zap_arc_default_runtime_context = {0};

zap_arc_runtime_context_t *zap_arc_default_context(void) {
  return &zap_arc_default_runtime_context;
}

static void
zap_arc_context_release_storage(zap_arc_runtime_context_t *context) {
  if (!context) {
    return;
  }
  for (size_t i = 0; i < context->root_count; ++i) {
    zap_arc_header_t *header = (zap_arc_header_t *)context->roots[i];
    if (header) {
      header->gc_mark &= (uint8_t)~ZAP_ARC_GC_BUFFERED;
    }
  }
  free(context->roots);
  free(context->snap);
  free(context->worklist);
  free(context->incoming);
  free(context->reachable);
  free(context->stack);
  free(context->map.keys);
  free(context->map.vals);
  *context = (zap_arc_runtime_context_t){0};
}

zap_arc_runtime_context_t *zap_arc_context_create(void) {
  return (zap_arc_runtime_context_t *)zap_runtime_calloc_array(
      1, sizeof(zap_arc_runtime_context_t));
}

void zap_arc_context_destroy(zap_arc_runtime_context_t *context) {
  if (!context || context == &zap_arc_default_runtime_context) {
    return;
  }
  zap_arc_context_release_storage(context);
  free(context);
}

void zap_arc_add_possible_root(zap_arc_runtime_context_t *context,
                               void *object) {
  if (!context || !object) {
    return;
  }
  zap_arc_header_t *header = (zap_arc_header_t *)object;
  if (!header->alive ||
      (header->gc_mark &
       (ZAP_ARC_GC_BUFFERED | ZAP_ARC_GC_GARBAGE | ZAP_ARC_GC_FINALIZING))) {
    return;
  }

  if (context->root_count == context->root_capacity) {
    size_t next_capacity =
        zap_runtime_next_capacity(context->root_capacity, 16);
    context->roots = (void **)zap_runtime_realloc_array(
        context->roots, next_capacity, sizeof(void *));
    context->root_capacity = next_capacity;
  }

  header->gc_mark |= ZAP_ARC_GC_BUFFERED;
  context->roots[context->root_count++] = object;
  if (context->root_count >= ZAP_ARC_COLLECTION_ROOT_THRESHOLD) {
    context->collection_pending = 1;
  }
#if defined(ZAP_RUNTIME_INSTRUMENTATION)
  ++zap_runtime_ownership_counters.candidate_roots;
#endif
}

void zap_arc_collect_at_safepoint(zap_arc_runtime_context_t *context) {
  if (!context || !context->collection_pending || context->collecting) {
    return;
  }
  context->collection_pending = 0;
  zap_arc_cycle_collect(context);
}

void zap_arc_remove_possible_root(zap_arc_runtime_context_t *context,
                                  void *object) {
  if (!context || !object) {
    return;
  }
  zap_arc_header_t *header = (zap_arc_header_t *)object;
  if (!(header->gc_mark & ZAP_ARC_GC_BUFFERED)) {
    return;
  }
  header->gc_mark &= (uint8_t)~ZAP_ARC_GC_BUFFERED;
  for (size_t i = 0; i < context->root_count; ++i) {
    if (context->roots[i] == object) {
      --context->root_count;
      context->roots[i] = context->roots[context->root_count];
      context->roots[context->root_count] = NULL;
      break;
    }
  }
}

void zap_arc_deallocate(zap_arc_runtime_context_t *context, void *object) {
  if (!object) {
    return;
  }
  zap_arc_remove_possible_root(context, object);
  free(object);
}

static size_t zap_arc_hash_ptr(void *p) {
  uintptr_t x = (uintptr_t)p;
  x ^= x >> 33;
  x *= (uintptr_t)0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  return (size_t)x;
}

static void zap_arc_ptrmap_init(zap_arc_ptrmap_t *m, size_t cap) {
  m->cap = cap;
  m->len = 0;
  m->keys = (void **)zap_runtime_calloc_array(cap, sizeof(void *));
  m->vals = (uint32_t *)zap_runtime_calloc_array(cap, sizeof(uint32_t));
}

static void zap_arc_ptrmap_clear(zap_arc_ptrmap_t *m) {
  if (m->keys) {
    memset(m->keys, 0, m->cap * sizeof(void *));
  }
  m->len = 0;
}

static int zap_arc_ptrmap_get(const zap_arc_ptrmap_t *m, void *key,
                              uint32_t *out_index) {
  size_t mask = m->cap - 1;
  size_t i = zap_arc_hash_ptr(key) & mask;
  while (m->keys[i]) {
    if (m->keys[i] == key) {
      *out_index = m->vals[i];
      return 1;
    }
    i = (i + 1) & mask;
  }
  return 0;
}

static void zap_arc_ptrmap_grow(zap_arc_ptrmap_t *m) {
  size_t ncap = zap_runtime_next_capacity(m->cap, 16);
  void **nkeys = (void **)zap_runtime_calloc_array(ncap, sizeof(void *));
  uint32_t *nvals =
      (uint32_t *)zap_runtime_calloc_array(ncap, sizeof(uint32_t));
  size_t nmask = ncap - 1;
  for (size_t i = 0; i < m->cap; ++i) {
    if (!m->keys[i]) {
      continue;
    }
    size_t j = zap_arc_hash_ptr(m->keys[i]) & nmask;
    while (nkeys[j]) {
      j = (j + 1) & nmask;
    }
    nkeys[j] = m->keys[i];
    nvals[j] = m->vals[i];
  }
  free(m->keys);
  free(m->vals);
  m->keys = nkeys;
  m->vals = nvals;
  m->cap = ncap;
}

static void zap_arc_ptrmap_put(zap_arc_ptrmap_t *m, void *key, uint32_t value) {
  if ((m->len + 1) * 2 >= m->cap) {
    zap_arc_ptrmap_grow(m);
  }
  size_t mask = m->cap - 1;
  size_t i = zap_arc_hash_ptr(key) & mask;
  while (m->keys[i]) {
    if (m->keys[i] == key) {
      m->vals[i] = value;
      return;
    }
    i = (i + 1) & mask;
  }
  m->keys[i] = key;
  m->vals[i] = value;
  m->len++;
}

static void zap_arc_ws_push(void ***ws, size_t *count, size_t *cap,
                            zap_arc_ptrmap_t *map, void *object) {
  uint32_t existing;
  if (zap_arc_ptrmap_get(map, object, &existing)) {
    return;
  }
  if (*count > UINT32_MAX) {
    zap_arc_graph_limit_exceeded();
  }
  if (*count == *cap) {
    size_t ncap = zap_runtime_next_capacity(*cap, 32);
    void **next = (void **)zap_runtime_realloc_array(*ws, ncap, sizeof(void *));
    *ws = next;
    *cap = ncap;
  }
  zap_arc_ptrmap_put(map, object, (uint32_t)*count);
  (*ws)[(*count)++] = object;
}

static void zap_arc_ensure_scratch(zap_arc_runtime_context_t *context,
                                   size_t n) {
  if (n <= context->scratch_cap) {
    return;
  }
#if defined(ZAP_RUNTIME_INSTRUMENTATION)
  if (zap_runtime_fail_arc_scratch_allocation) {
    zap_runtime_fail_arc_scratch_allocation = 0;
    zap_runtime_out_of_memory();
  }
#endif
  size_t ncap = context->scratch_cap ? context->scratch_cap : 32;
  while (ncap < n) {
    ncap = zap_runtime_next_capacity(ncap, 32);
  }
  int *ni =
      (int *)zap_runtime_realloc_array(context->incoming, ncap, sizeof(int));
  uint8_t *nr = (uint8_t *)zap_runtime_realloc_array(context->reachable, ncap,
                                                     sizeof(uint8_t));
  uint32_t *ns = (uint32_t *)zap_runtime_realloc_array(context->stack, ncap,
                                                       sizeof(uint32_t));
  context->incoming = ni;
  context->reachable = nr;
  context->stack = ns;
  context->scratch_cap = ncap;
}

typedef struct {
  void ***worklist;
  size_t *count;
  size_t *capacity;
  zap_arc_ptrmap_t *map;
} zap_arc_discover_context_t;

static void zap_arc_discover_child(void *context, void *child) {
  zap_arc_discover_context_t *state = (zap_arc_discover_context_t *)context;
  if (child) {
    zap_arc_ws_push(state->worklist, state->count, state->capacity, state->map,
                    child);
  }
}

typedef struct {
  const zap_arc_ptrmap_t *map;
  int *incoming;
} zap_arc_incoming_context_t;

static void zap_arc_count_incoming(void *context, void *child) {
  zap_arc_incoming_context_t *state = (zap_arc_incoming_context_t *)context;
  uint32_t child_index;
  if (child && zap_arc_ptrmap_get(state->map, child, &child_index)) {
    state->incoming[child_index] += 1;
  }
}

typedef struct {
  const zap_arc_ptrmap_t *map;
  uint8_t *reachable;
  uint32_t *stack;
  size_t *stack_count;
} zap_arc_reachable_context_t;

static void zap_arc_mark_reachable(void *context, void *child) {
  zap_arc_reachable_context_t *state = (zap_arc_reachable_context_t *)context;
  uint32_t child_index;
  if (child && zap_arc_ptrmap_get(state->map, child, &child_index) &&
      !state->reachable[child_index]) {
    state->reachable[child_index] = 1;
    state->stack[(*state->stack_count)++] = child_index;
  }
}

void zap_arc_cycle_collect(zap_arc_runtime_context_t *context) {
  if (!context || context->collecting || context->root_count == 0) {
    return;
  }
  context->collecting = 1;
#if defined(ZAP_RUNTIME_INSTRUMENTATION)
  ++zap_runtime_ownership_counters.collection_runs;
#endif

  if (context->root_count > context->snap_cap) {
    void **next = (void **)zap_runtime_realloc_array(
        context->snap, context->root_count, sizeof(void *));
    context->snap = next;
    context->snap_cap = context->root_count;
  }
  void **roots = context->snap;
  size_t root_count = 0;
  for (size_t i = 0; i < context->root_count; ++i) {
    void *object = context->roots[i];
    if (!object) {
      continue;
    }
    zap_arc_header_t *header = (zap_arc_header_t *)object;
    header->gc_mark &= (uint8_t)~ZAP_ARC_GC_BUFFERED;
    roots[root_count++] = object;
  }
  context->root_count = 0;
  context->collection_pending = 0;
  if (root_count == 0) {
    context->collecting = 0;
    return;
  }

  if (context->map.cap == 0) {
    zap_arc_ptrmap_init(&context->map, 64);
  }
  zap_arc_ptrmap_clear(&context->map);

  size_t ws_count = 0;
  for (size_t i = 0; i < root_count; ++i) {
    zap_arc_ws_push(&context->worklist, &ws_count, &context->worklist_cap,
                    &context->map, roots[i]);
  }
  for (size_t cursor = 0; cursor < ws_count; ++cursor) {
    zap_arc_header_t *header = (zap_arc_header_t *)context->worklist[cursor];
    if (!header->alive || !header->metadata) {
      continue;
    }
    if (header->metadata->trace_fn) {
      zap_arc_discover_context_t discovery = {
          &context->worklist, &ws_count, &context->worklist_cap, &context->map};
      header->metadata->trace_fn(context->worklist[cursor],
                                 zap_arc_discover_child, &discovery);
    }
  }

#if defined(ZAP_RUNTIME_INSTRUMENTATION)
  zap_runtime_ownership_counters.visited_objects += ws_count;
#endif

  zap_arc_ensure_scratch(context, ws_count);
  int *incoming = context->incoming;
  uint8_t *reachable = context->reachable;
  uint32_t *stack = context->stack;
  memset(incoming, 0, ws_count * sizeof(int));
  memset(reachable, 0, ws_count * sizeof(uint8_t));

  for (size_t i = 0; i < ws_count; ++i) {
    zap_arc_header_t *header = (zap_arc_header_t *)context->worklist[i];
    if (!header->alive || !header->metadata) {
      continue;
    }
    if (header->metadata->trace_fn) {
      zap_arc_incoming_context_t incoming_context = {&context->map, incoming};
      header->metadata->trace_fn(context->worklist[i], zap_arc_count_incoming,
                                 &incoming_context);
    }
  }

  size_t sp = 0;
  for (size_t i = 0; i < ws_count; ++i) {
    zap_arc_header_t *header = (zap_arc_header_t *)context->worklist[i];
    if (header->alive && header->strong_count > incoming[i] && !reachable[i]) {
      reachable[i] = 1;
      stack[sp++] = (uint32_t)i;
    }
  }
  while (sp) {
    uint32_t idx = stack[--sp];
    zap_arc_header_t *header = (zap_arc_header_t *)context->worklist[idx];
    if (!header->alive || !header->metadata) {
      continue;
    }
    if (header->metadata->trace_fn) {
      zap_arc_reachable_context_t reachable_context = {&context->map, reachable,
                                                       stack, &sp};
      header->metadata->trace_fn(context->worklist[idx], zap_arc_mark_reachable,
                                 &reachable_context);
    }
  }

  for (size_t i = 0; i < ws_count; ++i) {
    zap_arc_header_t *header = (zap_arc_header_t *)context->worklist[i];
    if (!header->alive || reachable[i]) {
      continue;
    }
    header->gc_mark = ZAP_ARC_GC_GARBAGE;
    header->alive = 0;
#if defined(ZAP_RUNTIME_INSTRUMENTATION)
    ++zap_runtime_ownership_counters.reclaimed_objects;
#endif
  }
  for (size_t i = 0; i < ws_count; ++i) {
    zap_arc_header_t *header = (zap_arc_header_t *)context->worklist[i];
    if ((header->gc_mark & ZAP_ARC_GC_GARBAGE) && header->destroy_fn) {
      header->destroy_fn(context->worklist[i]);
    }
  }
  for (size_t i = 0; i < ws_count; ++i) {
    zap_arc_header_t *header = (zap_arc_header_t *)context->worklist[i];
    if ((header->gc_mark & ZAP_ARC_GC_GARBAGE) && header->weak_count == 0) {
      zap_arc_deallocate(context, context->worklist[i]);
    } else if (header->gc_mark & ZAP_ARC_GC_GARBAGE) {
      header->gc_mark &= (uint8_t)~ZAP_ARC_GC_GARBAGE;
    }
  }

  context->collecting = 0;
}

__attribute__((destructor)) static void zap_arc_shutdown(void) {
  zap_arc_context_release_storage(zap_arc_default_context());
}

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

static char *zap_string_alloc_owned(size_t len);

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

static char *zap_string_alloc_owned(size_t len) {
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

static void zap_string_release_ptr(const char *ptr) {
  zap_string_header_t *header = zap_string_header_from_ptr(ptr);
  if (!header || header->refs == ZAP_STRING_IMMORTAL_REFCOUNT) {
    return;
  }
  header->refs -= 1;
  if (header->refs <= 0) {
    free(header);
  }
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

static long zap_process_argc = 0;
static char **zap_process_argv = NULL;

void __zap_process_set_args(int argc, char **argv) {
  zap_process_argc = argc;
  zap_process_argv = argv;
}

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

zap_string_t getln() {
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
    return (zap_string_t){.ptr = NULL, .len = 0};
  }

  const char *arg = zap_process_argv[i];
  return zap_string_from_cstr(arg);
}

zap_string_t zap_process_getenv(zap_string_t name) {
  char *name_buffer = zap_string_to_cstr(name);
  if (!name_buffer) {
    return (zap_string_t){.ptr = NULL, .len = 0};
  }

  const char *value = getenv(name_buffer);
  zap_string_t result = zap_string_from_cstr(value);
  free(name_buffer);
  return result;
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
    return (zap_string_t){.ptr = NULL, .len = 0};
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

static int zap_net_bind_addrinfo(const char *host, long port, int socktype,
                                 int flags, struct addrinfo **out) {
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

_Bool zap_fs_exists(zap_string_t path) {
  struct stat st;
  return zap_stat_path(path, &st) == 0;
}

_Bool zap_fs_is_file(zap_string_t path) {
  struct stat st;
  if (zap_stat_path(path, &st) != 0) {
    return 0;
  }

  return S_ISREG(st.st_mode);
}

_Bool zap_fs_is_dir(zap_string_t path) {
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

zap_string_t zap_fs_read_file(zap_string_t path) {
  char *buffer = zap_copy_path(path);
  if (!buffer) {
    zap_fs_last_error_code = ENOMEM;
    return (zap_string_t){.ptr = NULL, .len = 0};
  }

  FILE *file = fopen(buffer, "rb");
  free(buffer);
  if (!file) {
    zap_fs_last_error_code = errno;
    return (zap_string_t){.ptr = NULL, .len = 0};
  }

  if (fseek(file, 0, SEEK_END) != 0) {
    zap_fs_last_error_code = errno;
    fclose(file);
    return (zap_string_t){.ptr = NULL, .len = 0};
  }

  long size = ftell(file);
  if (size < 0) {
    zap_fs_last_error_code = errno;
    fclose(file);
    return (zap_string_t){.ptr = NULL, .len = 0};
  }

  if (fseek(file, 0, SEEK_SET) != 0) {
    zap_fs_last_error_code = errno;
    fclose(file);
    return (zap_string_t){.ptr = NULL, .len = 0};
  }

  char *content = zap_string_alloc_owned((size_t)size);
  if (!content) {
    zap_fs_last_error_code = ENOMEM;
    fclose(file);
    return (zap_string_t){.ptr = NULL, .len = 0};
  }

  size_t read = fread(content, 1, (size_t)size, file);
  fclose(file);
  if (read != (size_t)size) {
    zap_fs_last_error_code = EIO;
    zap_string_release_ptr(content);
    return (zap_string_t){.ptr = NULL, .len = 0};
  }

  content[size] = '\0';
  zap_fs_last_error_code = 0;
  return (zap_string_t){.ptr = content, .len = size};
}

long zap_fs_write_file(zap_string_t path, zap_string_t content) {
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

long zap_fs_last_error() { return zap_fs_last_error_code; }

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
    return (zap_string_t){.ptr = NULL, .len = 0};
  }

  size_t cap = (size_t)maxLen;
  char *buf = zap_string_alloc_owned(cap);
  if (!buf) {
    zap_net_last_error = ENOMEM;
    return (zap_string_t){.ptr = NULL, .len = 0};
  }

  ssize_t n;
  do {
    n = recv((int)fd, buf, cap, 0);
  } while (n < 0 && errno == EINTR);
  if (n < 0) {
    zap_net_last_error = errno;
    zap_string_release_ptr(buf);
    return (zap_string_t){.ptr = NULL, .len = 0};
  }

  buf[n] = '\0';
  zap_net_last_error = 0;
  return (zap_string_t){.ptr = buf, .len = (long)n};
}

zap_string_t netResolve(zap_string_t host) {
  if (!host.ptr || host.len == 0) {
    zap_net_last_error = EINVAL;
    return (zap_string_t){.ptr = NULL, .len = 0};
  }

  char *host_buf = zap_copy_path(host);
  if (!host_buf) {
    zap_net_last_error = ENOMEM;
    return (zap_string_t){.ptr = NULL, .len = 0};
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
    return (zap_string_t){.ptr = NULL, .len = 0};
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
    return (zap_string_t){.ptr = NULL, .len = 0};
  }

  zap_net_last_error = 0;
  return zap_string_from_ptrlen(ipbuf, (long)strlen(ipbuf));
}

long netLastError() { return zap_net_last_error; }

typedef struct {
  SSL_CTX *context;
  SSL *ssl;
  int fd;
} zap_tls_session_t;

static void zap_tls_session_free(zap_tls_session_t *session) {
  if (!session) {
    return;
  }
  if (session->ssl) {
    SSL_shutdown(session->ssl);
    SSL_free(session->ssl);
  }
  if (session->context) {
    SSL_CTX_free(session->context);
  }
  if (session->fd >= 0) {
    close(session->fd);
  }
  free(session);
}

long zap_tls_connect(zap_string_t host, long port) {
  const long fd = netConnect(host, port);
  if (fd < 0) {
    zap_tls_last_error_code = zap_net_last_error;
    return 0;
  }

  char *host_buffer = zap_copy_path(host);
  if (!host_buffer) {
    close((int)fd);
    zap_tls_last_error_code = ENOMEM;
    return 0;
  }

  zap_tls_session_t *session = calloc(1, sizeof(*session));
  if (!session) {
    free(host_buffer);
    close((int)fd);
    zap_tls_last_error_code = ENOMEM;
    return 0;
  }
  session->fd = (int)fd;
  session->context = SSL_CTX_new(TLS_client_method());
  if (!session->context ||
      SSL_CTX_set_default_verify_paths(session->context) != 1) {
    free(host_buffer);
    zap_tls_session_free(session);
    zap_tls_last_error_code = EIO;
    return 0;
  }
  SSL_CTX_set_verify(session->context, SSL_VERIFY_PEER, NULL);
  session->ssl = SSL_new(session->context);
  if (!session->ssl ||
      SSL_set_tlsext_host_name(session->ssl, host_buffer) != 1 ||
      SSL_set1_host(session->ssl, host_buffer) != 1 ||
      SSL_set_fd(session->ssl, session->fd) != 1 ||
      SSL_connect(session->ssl) != 1) {
    free(host_buffer);
    zap_tls_session_free(session);
    zap_tls_last_error_code = EIO;
    return 0;
  }
  free(host_buffer);

  if (SSL_get_verify_result(session->ssl) != X509_V_OK) {
    zap_tls_session_free(session);
    zap_tls_last_error_code = EACCES;
    return 0;
  }

  zap_tls_last_error_code = 0;
  return (long)(intptr_t)session;
}

long zap_tls_send(long handle, zap_string_t data) {
  zap_tls_session_t *session = (zap_tls_session_t *)(intptr_t)handle;
  if (!session || !session->ssl || !data.ptr) {
    zap_tls_last_error_code = EINVAL;
    return -1;
  }

  size_t total = 0;
  const size_t target = data.len > 0 ? (size_t)data.len : 0;
  while (total < target) {
    const size_t remaining = target - total;
    const int request = remaining > INT_MAX ? INT_MAX : (int)remaining;
    const int written = SSL_write(session->ssl, data.ptr + total, request);
    if (written <= 0) {
      zap_tls_last_error_code = EIO;
      return -1;
    }
    total += (size_t)written;
  }
  zap_tls_last_error_code = 0;
  return (long)total;
}

zap_string_t zap_tls_recv(long handle, long max_len) {
  zap_tls_session_t *session = (zap_tls_session_t *)(intptr_t)handle;
  if (!session || !session->ssl || max_len <= 0) {
    zap_tls_last_error_code = EINVAL;
    return (zap_string_t){.ptr = NULL, .len = 0};
  }

  const size_t capacity = (size_t)max_len;
  char *buffer = zap_string_alloc_owned(capacity);
  if (!buffer) {
    zap_tls_last_error_code = ENOMEM;
    return (zap_string_t){.ptr = NULL, .len = 0};
  }
  const int request = capacity > INT_MAX ? INT_MAX : (int)capacity;
  const int received = SSL_read(session->ssl, buffer, request);
  if (received <= 0) {
    const int ssl_error = SSL_get_error(session->ssl, received);
    zap_string_release_ptr(buffer);
    if (ssl_error == SSL_ERROR_ZERO_RETURN) {
      zap_tls_last_error_code = 0;
    } else {
      zap_tls_last_error_code = EIO;
    }
    return (zap_string_t){.ptr = NULL, .len = 0};
  }
  buffer[received] = '\0';
  zap_tls_last_error_code = 0;
  return (zap_string_t){.ptr = buffer, .len = received};
}

long zap_tls_close(long handle) {
  zap_tls_session_t *session = (zap_tls_session_t *)(intptr_t)handle;
  if (!session) {
    zap_tls_last_error_code = EINVAL;
    return EINVAL;
  }
  zap_tls_session_free(session);
  zap_tls_last_error_code = 0;
  return 0;
}

long zap_tls_last_error() { return zap_tls_last_error_code; }
