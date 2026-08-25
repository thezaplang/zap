#include "allocation_internal.h"
#include "arc_layout.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

void zap_runtime_out_of_memory(void) {
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

void zap_arc_interface_not_found(void) {
  fputs("zap runtime error: object does not implement the requested "
        "interface\n",
        stderr);
  abort();
}

void *zap_arc_resolve_interface_method(void *object, const char *interface_name,
                                       int64_t method_index) {
  const zap_arc_header_t *header = (const zap_arc_header_t *)object;
  const zap_arc_interface_entry_t *entry =
      (const zap_arc_interface_entry_t *)header->interface_table;
  if (entry) {
    for (; entry->interface_name; ++entry) {
      if (strcmp(entry->interface_name, interface_name) == 0) {
        int64_t slot = entry->method_slots[method_index];
        return header->vtable[slot];
      }
    }
  }
  zap_arc_interface_not_found();
  return NULL;
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
