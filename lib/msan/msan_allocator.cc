//===-- msan_allocator.cc --------------------------- ---------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file is a part of MemorySanitizer.
//
// MemorySanitizer allocator.
//===----------------------------------------------------------------------===//

#include "sanitizer_common/sanitizer_allocator.h"
#include "sanitizer_common/sanitizer_allocator_interface.h"
#include "msan.h"
#include "msan_allocator.h"
#include "msan_origin.h"
#include "msan_thread.h"
#include "msan_poisoning.h"

namespace __msan {

struct Metadata {
  uptr requested_size;
};

struct MsanMapUnmapCallback {
  void OnMap(uptr p, uptr size) const {}
  void OnUnmap(uptr p, uptr size) const {
    __msan_unpoison((void *)p, size);

    // We are about to unmap a chunk of user memory.
    // Mark the corresponding shadow memory as not needed.
    uptr shadow_p = MEM_TO_SHADOW(p);
    ReleaseMemoryPagesToOS(shadow_p, shadow_p + size);
    if (__msan_get_track_origins()) {
      uptr origin_p = MEM_TO_ORIGIN(p);
      ReleaseMemoryPagesToOS(origin_p, origin_p + size);
    }
  }
};

#if defined(__mips64)
  static const uptr kMaxAllowedMallocSize = 2UL << 30;
  static const uptr kRegionSizeLog = 20;
  static const uptr kNumRegions = SANITIZER_MMAP_RANGE_SIZE >> kRegionSizeLog;
  typedef TwoLevelByteMap<(kNumRegions >> 12), 1 << 12> ByteMap;
  typedef CompactSizeClassMap SizeClassMap;

  typedef SizeClassAllocator32<0, SANITIZER_MMAP_RANGE_SIZE, sizeof(Metadata),
                               SizeClassMap, kRegionSizeLog, ByteMap,
                               MsanMapUnmapCallback> PrimaryAllocator;

#elif defined(__x86_64__)
#if SANITIZER_LINUX && !defined(MSAN_LINUX_X86_64_OLD_MAPPING)
  static const uptr kAllocatorSpace = 0x700000000000ULL;
#else
  static const uptr kAllocatorSpace = 0x600000000000ULL;
#endif
  static const uptr kMaxAllowedMallocSize = 8UL << 30;

  struct AP64 {  // Allocator64 parameters. Deliberately using a short name.
    static const uptr kSpaceBeg = kAllocatorSpace;
    static const uptr kSpaceSize = 0x40000000000; // 4T.
    static const uptr kMetadataSize = sizeof(Metadata);
    typedef DefaultSizeClassMap SizeClassMap;
    typedef MsanMapUnmapCallback MapUnmapCallback;
    static const uptr kFlags = 0;
  };

  typedef SizeClassAllocator64<AP64> PrimaryAllocator;

#elif defined(__powerpc64__)
  static const uptr kMaxAllowedMallocSize = 2UL << 30;  // 2G

  struct AP64 {  // Allocator64 parameters. Deliberately using a short name.
    static const uptr kSpaceBeg = 0x300000000000;
    static const uptr kSpaceSize = 0x020000000000; // 2T.
    static const uptr kMetadataSize = sizeof(Metadata);
    typedef DefaultSizeClassMap SizeClassMap;
    typedef MsanMapUnmapCallback MapUnmapCallback;
    static const uptr kFlags = 0;
  };

  typedef SizeClassAllocator64<AP64> PrimaryAllocator;
#elif defined(__aarch64__)
  static const uptr kMaxAllowedMallocSize = 2UL << 30;  // 2G
  static const uptr kRegionSizeLog = 20;
  static const uptr kNumRegions = SANITIZER_MMAP_RANGE_SIZE >> kRegionSizeLog;
  typedef TwoLevelByteMap<(kNumRegions >> 12), 1 << 12> ByteMap;
  typedef CompactSizeClassMap SizeClassMap;

  typedef SizeClassAllocator32<0, SANITIZER_MMAP_RANGE_SIZE, sizeof(Metadata),
                               SizeClassMap, kRegionSizeLog, ByteMap,
                               MsanMapUnmapCallback> PrimaryAllocator;
#endif
typedef SizeClassAllocatorLocalCache<PrimaryAllocator> AllocatorCache;
typedef LargeMmapAllocator<MsanMapUnmapCallback> SecondaryAllocator;
typedef CombinedAllocator<PrimaryAllocator, AllocatorCache,
                          SecondaryAllocator> Allocator;

static Allocator allocator;
static AllocatorCache fallback_allocator_cache;
static SpinMutex fallback_mutex;

void MsanAllocatorInit() {
  allocator.Init(
      common_flags()->allocator_may_return_null,
      common_flags()->allocator_release_to_os_interval_ms);
}

AllocatorCache *GetAllocatorCache(MsanThreadLocalMallocStorage *ms) {
  CHECK(ms);
  CHECK_LE(sizeof(AllocatorCache), sizeof(ms->allocator_cache));
  return reinterpret_cast<AllocatorCache *>(ms->allocator_cache);
}

void MsanThreadLocalMallocStorage::CommitBack() {
  allocator.SwallowCache(GetAllocatorCache(this));
}

static void *MsanAllocate(StackTrace *stack, uptr size, uptr alignment,
                          bool zeroise) {
  if (size > kMaxAllowedMallocSize) {
    Report("WARNING: MemorySanitizer failed to allocate %p bytes\n",
           (void *)size);
    return allocator.ReturnNullOrDieOnBadRequest();
  }
  MsanThread *t = GetCurrentThread();
  void *allocated;
  if (t) {
    AllocatorCache *cache = GetAllocatorCache(&t->malloc_storage());
    allocated = allocator.Allocate(cache, size, alignment, false);
  } else {
    SpinMutexLock l(&fallback_mutex);
    AllocatorCache *cache = &fallback_allocator_cache;
    allocated = allocator.Allocate(cache, size, alignment, false);
  }
  Metadata *meta =
      reinterpret_cast<Metadata *>(allocator.GetMetaData(allocated));
  meta->requested_size = size;
  if (zeroise) {
    __msan_clear_and_unpoison(allocated, size);
  } else if (flags()->poison_in_malloc) {
    __msan_poison(allocated, size);
    if (__msan_get_track_origins()) {
      stack->tag = StackTrace::TAG_ALLOC;
      Origin o = Origin::CreateHeapOrigin(stack);
      __msan_set_origin(allocated, size, o.raw_id());
    }
  }
  MSAN_MALLOC_HOOK(allocated, size);
  return allocated;
}

void MsanDeallocate(StackTrace *stack, void *p) {
  CHECK(p);
  MSAN_FREE_HOOK(p);
  Metadata *meta = reinterpret_cast<Metadata *>(allocator.GetMetaData(p));
  uptr size = meta->requested_size;
  meta->requested_size = 0;
  // This memory will not be reused by anyone else, so we are free to keep it
  // poisoned.
  if (flags()->poison_in_free) {
    __msan_poison(p, size);
    if (__msan_get_track_origins()) {
      stack->tag = StackTrace::TAG_DEALLOC;
      Origin o = Origin::CreateHeapOrigin(stack);
      __msan_set_origin(p, size, o.raw_id());
    }
  }
  MsanThread *t = GetCurrentThread();
  if (t) {
    AllocatorCache *cache = GetAllocatorCache(&t->malloc_storage());
    allocator.Deallocate(cache, p);
  } else {
    SpinMutexLock l(&fallback_mutex);
    AllocatorCache *cache = &fallback_allocator_cache;
    allocator.Deallocate(cache, p);
  }
}

void *MsanCalloc(StackTrace *stack, uptr nmemb, uptr size) {
  if (CallocShouldReturnNullDueToOverflow(size, nmemb))
    return allocator.ReturnNullOrDieOnBadRequest();
  return MsanReallocate(stack, nullptr, nmemb * size, sizeof(u64), true);
}

void *MsanReallocate(StackTrace *stack, void *old_p, uptr new_size,
                     uptr alignment, bool zeroise) {
  if (!old_p)
    return MsanAllocate(stack, new_size, alignment, zeroise);
  if (!new_size) {
    MsanDeallocate(stack, old_p);
    return nullptr;
  }
  Metadata *meta = reinterpret_cast<Metadata*>(allocator.GetMetaData(old_p));
  uptr old_size = meta->requested_size;
  uptr actually_allocated_size = allocator.GetActuallyAllocatedSize(old_p);
  if (new_size <= actually_allocated_size) {
    // We are not reallocating here.
    meta->requested_size = new_size;
    if (new_size > old_size) {
      if (zeroise) {
        __msan_clear_and_unpoison((char *)old_p + old_size,
                                  new_size - old_size);
      } else if (flags()->poison_in_malloc) {
        stack->tag = StackTrace::TAG_ALLOC;
        PoisonMemory((char *)old_p + old_size, new_size - old_size, stack);
      }
    }
    return old_p;
  }
  uptr memcpy_size = Min(new_size, old_size);
  void *new_p = MsanAllocate(stack, new_size, alignment, zeroise);
  // Printf("realloc: old_size %zd new_size %zd\n", old_size, new_size);
  if (new_p) {
    CopyMemory(new_p, old_p, memcpy_size, stack);
    MsanDeallocate(stack, old_p);
  }
  return new_p;
}

static uptr AllocationSize(const void *p) {
  if (!p) return 0;
  const void *beg = allocator.GetBlockBegin(p);
  if (beg != p) return 0;
  Metadata *b = (Metadata *)allocator.GetMetaData(p);
  return b->requested_size;
}

} // namespace __msan

using namespace __msan;

uptr __sanitizer_get_current_allocated_bytes() {
  uptr stats[AllocatorStatCount];
  allocator.GetStats(stats);
  return stats[AllocatorStatAllocated];
}

uptr __sanitizer_get_heap_size() {
  uptr stats[AllocatorStatCount];
  allocator.GetStats(stats);
  return stats[AllocatorStatMapped];
}

uptr __sanitizer_get_free_bytes() { return 1; }

uptr __sanitizer_get_unmapped_bytes() { return 1; }

uptr __sanitizer_get_estimated_allocated_size(uptr size) { return size; }

int __sanitizer_get_ownership(const void *p) { return AllocationSize(p) != 0; }

uptr __sanitizer_get_allocated_size(const void *p) { return AllocationSize(p); }
