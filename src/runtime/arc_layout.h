#pragma once

#include <stddef.h>
#include <stdint.h>

// Shared ARC object header ABI used by runtime (C) and codegen (C++).
#define ZAP_ARC_ABI_VERSION 6
#define ZAP_ARC_STRONG_COUNT_INDEX 0
#define ZAP_ARC_WEAK_COUNT_INDEX 1
#define ZAP_ARC_ALIVE_INDEX 2
#define ZAP_ARC_GC_MARK_INDEX 3
#define ZAP_ARC_RELEASE_FN_INDEX 4
#define ZAP_ARC_DESTROY_FN_INDEX 5
#define ZAP_ARC_METADATA_INDEX 6
#define ZAP_ARC_VTABLE_INDEX 7
#define ZAP_ARC_INTERFACE_TABLE_INDEX 8
#define ZAP_ARC_FIELD_START_INDEX 9
#define ZAP_ARC_HEADER_FIELD_COUNT ZAP_ARC_FIELD_START_INDEX
// A self-cycle has one possible root, so a larger threshold would postpone its
// finalization indefinitely when no other managed object is released.
#define ZAP_ARC_COLLECTION_ROOT_THRESHOLD 1

// Flag bits packed into the gc_mark byte (index 3).
#define ZAP_ARC_GC_GARBAGE 0x1
#define ZAP_ARC_GC_BUFFERED 0x2
#define ZAP_ARC_GC_FINALIZING 0x4

typedef void (*zap_arc_trace_visitor_t)(void *context, void *child);
typedef void (*zap_arc_trace_fn_t)(void *object,
                                   zap_arc_trace_visitor_t visitor,
                                   void *context);

typedef struct zap_arc_metadata_t {
  zap_arc_trace_fn_t trace_fn;
} zap_arc_metadata_t;

typedef struct zap_arc_runtime_context_t zap_arc_runtime_context_t;

typedef struct zap_arc_header_t {
  int64_t strong_count;
  int64_t weak_count;
  uint8_t alive;
  uint8_t gc_mark;
  void (*release_fn)(void *);
  // Finalizes the object and drops its fields; storage is deallocated
  // separately.
  void (*destroy_fn)(void *);
  const zap_arc_metadata_t *metadata;
  void **vtable;
  const void *interface_table;
} zap_arc_header_t;

typedef struct zap_arc_interface_entry_t {
  const char *interface_name;
  const int64_t *method_slots;
} zap_arc_interface_entry_t;

#if defined(ZAP_RUNTIME_INSTRUMENTATION)
typedef struct zap_runtime_ownership_counters_t {
  uint64_t allocations;
  uint64_t strong_retain_calls;
  uint64_t strong_release_calls;
  uint64_t copy_operations;
  uint64_t drop_operations;
  uint64_t destroy_calls;
  uint64_t candidate_roots;
  uint64_t collection_runs;
  uint64_t visited_objects;
  uint64_t reclaimed_objects;
} zap_runtime_ownership_counters_t;
#endif

#if defined(__cplusplus)
#define ZAP_ARC_STATIC_ASSERT(condition, message)                              \
  static_assert(condition, message)
extern "C" {
#else
#define ZAP_ARC_STATIC_ASSERT(condition, message)                              \
  _Static_assert(condition, message)
#endif

zap_arc_runtime_context_t *zap_arc_default_context(void);
// Creates an isolated, single-threaded ARC collector context.
zap_arc_runtime_context_t *zap_arc_context_create(void);
// Destroys a context created by zap_arc_context_create(). The default context
// is owned by the runtime and is intentionally not destroyed by this API.
// Destroying a context drops its outstanding possible-root registrations.
void zap_arc_context_destroy(zap_arc_runtime_context_t *context);
void zap_arc_add_possible_root(zap_arc_runtime_context_t *context,
                               void *object);
void zap_arc_remove_possible_root(zap_arc_runtime_context_t *context,
                                  void *object);
void zap_arc_deallocate(zap_arc_runtime_context_t *context, void *object);
void zap_arc_cycle_collect(zap_arc_runtime_context_t *context);
// Runs scheduled collection at a non-destructor function-return safe point.
void zap_arc_collect_at_safepoint(zap_arc_runtime_context_t *context);
void *zap_runtime_alloc(size_t size);
void zap_arc_strong_refcount_overflow(void);
void zap_arc_weak_refcount_overflow(void);
void zap_arc_strong_refcount_underflow(void);
void zap_arc_weak_refcount_underflow(void);
void zap_arc_retain_dead_object(void);
void zap_arc_interface_not_found(void);
void *zap_arc_resolve_interface_method(void *object, const char *interface_name,
                                       int64_t method_index);

#if defined(ZAP_RUNTIME_INSTRUMENTATION)
void zap_runtime_ownership_reset_counters(void);
void zap_runtime_ownership_snapshot_counters(
    zap_runtime_ownership_counters_t *out);
void zap_runtime_ownership_note_strong_retain(void);
void zap_runtime_ownership_note_strong_release(void);
void zap_runtime_ownership_note_copy(void);
void zap_runtime_ownership_note_drop(void);
void zap_runtime_ownership_note_destroy(void);
uint64_t zap_runtime_ownership_copy_operations(void);
uint64_t zap_runtime_ownership_drop_operations(void);
uint64_t zap_runtime_ownership_destroy_calls(void);
// Test-only fault injection for the collector's scratch-storage allocation.
void zap_runtime_test_fail_next_arc_scratch_allocation(void);
#endif

#if defined(__cplusplus)
}
#endif

ZAP_ARC_STATIC_ASSERT(ZAP_ARC_STRONG_COUNT_INDEX == 0,
                      "ARC ABI: strong_count index mismatch");
ZAP_ARC_STATIC_ASSERT(ZAP_ARC_WEAK_COUNT_INDEX == 1,
                      "ARC ABI: weak_count index mismatch");
ZAP_ARC_STATIC_ASSERT(ZAP_ARC_ALIVE_INDEX == 2,
                      "ARC ABI: alive index mismatch");
ZAP_ARC_STATIC_ASSERT(ZAP_ARC_GC_MARK_INDEX == 3,
                      "ARC ABI: gc_mark index mismatch");
ZAP_ARC_STATIC_ASSERT(ZAP_ARC_RELEASE_FN_INDEX == 4,
                      "ARC ABI: release_fn index mismatch");
ZAP_ARC_STATIC_ASSERT(ZAP_ARC_DESTROY_FN_INDEX == 5,
                      "ARC ABI: destroy_fn index mismatch");
ZAP_ARC_STATIC_ASSERT(ZAP_ARC_METADATA_INDEX == 6,
                      "ARC ABI: metadata index mismatch");
ZAP_ARC_STATIC_ASSERT(ZAP_ARC_VTABLE_INDEX == 7,
                      "ARC ABI: vtable index mismatch");
ZAP_ARC_STATIC_ASSERT(ZAP_ARC_HEADER_FIELD_COUNT == ZAP_ARC_FIELD_START_INDEX,
                      "ARC ABI: header field count mismatch");
ZAP_ARC_STATIC_ASSERT(offsetof(zap_arc_header_t, strong_count) == 0,
                      "ARC ABI: strong_count offset mismatch");
ZAP_ARC_STATIC_ASSERT(offsetof(zap_arc_header_t, weak_count) >
                          offsetof(zap_arc_header_t, strong_count),
                      "ARC ABI: weak_count must be after strong_count");
ZAP_ARC_STATIC_ASSERT(offsetof(zap_arc_header_t, alive) >
                          offsetof(zap_arc_header_t, weak_count),
                      "ARC ABI: alive must be after weak_count");
ZAP_ARC_STATIC_ASSERT(offsetof(zap_arc_header_t, gc_mark) >
                          offsetof(zap_arc_header_t, alive),
                      "ARC ABI: gc_mark must be after alive");
ZAP_ARC_STATIC_ASSERT(offsetof(zap_arc_header_t, release_fn) >
                          offsetof(zap_arc_header_t, gc_mark),
                      "ARC ABI: release_fn must be after gc_mark");
ZAP_ARC_STATIC_ASSERT(offsetof(zap_arc_header_t, destroy_fn) >
                          offsetof(zap_arc_header_t, release_fn),
                      "ARC ABI: destroy_fn must be after release_fn");
ZAP_ARC_STATIC_ASSERT(offsetof(zap_arc_header_t, metadata) >
                          offsetof(zap_arc_header_t, destroy_fn),
                      "ARC ABI: metadata must be after destroy_fn");
ZAP_ARC_STATIC_ASSERT(offsetof(zap_arc_header_t, vtable) >
                          offsetof(zap_arc_header_t, metadata),
                      "ARC ABI: vtable must be after metadata");
ZAP_ARC_STATIC_ASSERT(offsetof(zap_arc_header_t, interface_table) >
                          offsetof(zap_arc_header_t, vtable),
                      "ARC ABI: interface_table must be after vtable");

#undef ZAP_ARC_STATIC_ASSERT
