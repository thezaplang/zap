#pragma once
#include "../runtime/arc_layout.h"

namespace codegen {

// Shared layout of the ARC class object header.
constexpr unsigned kClassStrongCountIndex = ZAP_ARC_STRONG_COUNT_INDEX;
constexpr unsigned kClassWeakCountIndex = ZAP_ARC_WEAK_COUNT_INDEX;
constexpr unsigned kClassAliveIndex = ZAP_ARC_ALIVE_INDEX;
constexpr unsigned kClassGcMarkIndex = ZAP_ARC_GC_MARK_INDEX;
constexpr unsigned kClassReleaseFnIndex = ZAP_ARC_RELEASE_FN_INDEX;
constexpr unsigned kClassDestroyFnIndex = ZAP_ARC_DESTROY_FN_INDEX;
constexpr unsigned kClassMetadataIndex = ZAP_ARC_METADATA_INDEX;
constexpr unsigned kClassVTableIndex = ZAP_ARC_VTABLE_INDEX;
constexpr unsigned kClassFieldStartIndex = ZAP_ARC_FIELD_START_INDEX;

// Flag bits packed into the gc_mark byte (see arc_layout.h).
constexpr unsigned kClassGcGarbageMask = ZAP_ARC_GC_GARBAGE;
constexpr unsigned kClassGcBufferedMask = ZAP_ARC_GC_BUFFERED;

} // namespace codegen
