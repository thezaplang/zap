#pragma once

// Shared ARC object header layout contract used by runtime (C)
// and codegen (C++).
#define ZAP_ARC_STRONG_COUNT_INDEX 0
#define ZAP_ARC_WEAK_COUNT_INDEX 1
#define ZAP_ARC_ALIVE_INDEX 2
#define ZAP_ARC_GC_MARK_INDEX 3
#define ZAP_ARC_RELEASE_FN_INDEX 4
#define ZAP_ARC_DESTROY_FN_INDEX 5
#define ZAP_ARC_METADATA_INDEX 6
#define ZAP_ARC_VTABLE_INDEX 7
#define ZAP_ARC_FIELD_START_INDEX 8

// Flag bits packed into the gc_mark byte (index 3).
#define ZAP_ARC_GC_GARBAGE 0x1
#define ZAP_ARC_GC_BUFFERED 0x2
