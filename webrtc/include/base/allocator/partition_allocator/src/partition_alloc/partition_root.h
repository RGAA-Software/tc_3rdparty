// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_ALLOCATOR_PARTITION_ALLOCATOR_SRC_PARTITION_ALLOC_PARTITION_ROOT_H_
#define BASE_ALLOCATOR_PARTITION_ALLOCATOR_SRC_PARTITION_ALLOC_PARTITION_ROOT_H_

// DESCRIPTION
// PartitionRoot::Alloc() and PartitionRoot::Free() are approximately analogous
// to malloc() and free().
//
// The main difference is that a PartitionRoot object must be supplied to these
// functions, representing a specific "heap partition" that will be used to
// satisfy the allocation. Different partitions are guaranteed to exist in
// separate address spaces, including being separate from the main system
// heap. If the contained objects are all freed, physical memory is returned to
// the system but the address space remains reserved.  See PartitionAlloc.md for
// other security properties PartitionAlloc provides.
//
// THE ONLY LEGITIMATE WAY TO OBTAIN A PartitionRoot IS THROUGH THE
// PartitionAllocator classes. To minimize the instruction count to the fullest
// extent possible, the PartitionRoot is really just a header adjacent to other
// data areas provided by the allocator class.
//
// The constraints for PartitionRoot::Alloc() are:
// - Multi-threaded use against a single partition is ok; locking is handled.
// - Allocations of any arbitrary size can be handled (subject to a limit of
//   INT_MAX bytes for security reasons).
// - Bucketing is by approximate size, for example an allocation of 4000 bytes
//   might be placed into a 4096-byte bucket. Bucket sizes are chosen to try and
//   keep worst-case waste to ~10%.

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

#include "build/build_config.h"
#include "partition_alloc/address_pool_manager_types.h"
#include "partition_alloc/allocation_guard.h"
#include "partition_alloc/chromecast_buildflags.h"
#include "partition_alloc/freeslot_bitmap.h"
#include "partition_alloc/lightweight_quarantine.h"
#include "partition_alloc/page_allocator.h"
#include "partition_alloc/partition_address_space.h"
#include "partition_alloc/partition_alloc-inl.h"
#include "partition_alloc/partition_alloc_allocation_data.h"
#include "partition_alloc/partition_alloc_base/bits.h"
#include "partition_alloc/partition_alloc_base/compiler_specific.h"
#include "partition_alloc/partition_alloc_base/component_export.h"
#include "partition_alloc/partition_alloc_base/debug/debugging_buildflags.h"
#include "partition_alloc/partition_alloc_base/export_template.h"
#include "partition_alloc/partition_alloc_base/no_destructor.h"
#include "partition_alloc/partition_alloc_base/notreached.h"
#include "partition_alloc/partition_alloc_base/thread_annotations.h"
#include "partition_alloc/partition_alloc_base/time/time.h"
#include "partition_alloc/partition_alloc_buildflags.h"
#include "partition_alloc/partition_alloc_check.h"
#include "partition_alloc/partition_alloc_config.h"
#include "partition_alloc/partition_alloc_constants.h"
#include "partition_alloc/partition_alloc_forward.h"
#include "partition_alloc/partition_alloc_hooks.h"
#include "partition_alloc/partition_bucket.h"
#include "partition_alloc/partition_bucket_lookup.h"
#include "partition_alloc/partition_cookie.h"
#include "partition_alloc/partition_direct_map_extent.h"
#include "partition_alloc/partition_freelist_entry.h"
#include "partition_alloc/partition_lock.h"
#include "partition_alloc/partition_oom.h"
#include "partition_alloc/partition_page.h"
#include "partition_alloc/partition_ref_count.h"
#include "partition_alloc/reservation_offset_table.h"
#include "partition_alloc/tagging.h"
#include "partition_alloc/thread_cache.h"
#include "partition_alloc/thread_isolation/thread_isolation.h"

#if BUILDFLAG(USE_STARSCAN)
#include "partition_alloc/starscan/pcscan.h"
#endif

namespace partition_alloc::internal {

// We want this size to be big enough that we have time to start up other
// scripts _before_ we wrap around.
static constexpr size_t kAllocInfoSize = 1 << 24;

struct AllocInfo {
  std::atomic<size_t> index{0};
  struct {
    uintptr_t addr;
    size_t size;
  } allocs[kAllocInfoSize] = {};
};

#if BUILDFLAG(RECORD_ALLOC_INFO)
extern AllocInfo g_allocs;

void RecordAllocOrFree(uintptr_t addr, size_t size);
#endif  // BUILDFLAG(RECORD_ALLOC_INFO)
}  // namespace partition_alloc::internal

namespace partition_alloc {

namespace internal {
// Avoid including partition_address_space.h from this .h file, by moving the
// call to IsManagedByPartitionAllocBRPPool into the .cc file.
#if BUILDFLAG(PA_DCHECK_IS_ON)
PA_COMPONENT_EXPORT(PARTITION_ALLOC)
void DCheckIfManagedByPartitionAllocBRPPool(uintptr_t address);
#else
PA_ALWAYS_INLINE void DCheckIfManagedByPartitionAllocBRPPool(
    uintptr_t address) {}
#endif

#if PA_CONFIG(USE_PARTITION_ROOT_ENUMERATOR)
class PartitionRootEnumerator;
#endif

}  // namespace internal

// Bit flag constants used to purge memory.  See PartitionRoot::PurgeMemory.
//
// In order to support bit operations like `flag_a | flag_b`, the old-fashioned
// enum (+ surrounding named struct) is used instead of enum class.
struct PurgeFlags {
  enum : int {
    // Decommitting the ring list of empty slot spans is reasonably fast.
    kDecommitEmptySlotSpans = 1 << 0,
    // Discarding unused system pages is slower, because it involves walking all
    // freelists in all active slot spans of all buckets >= system page
    // size. It often frees a similar amount of memory to decommitting the empty
    // slot spans, though.
    kDiscardUnusedSystemPages = 1 << 1,
    // Aggressively reclaim memory. This is meant to be used in low-memory
    // situations, not for periodic memory reclaiming.
    kAggressiveReclaim = 1 << 2,
  };
};

// Options struct used to configure PartitionRoot and PartitionAllocator.
struct PartitionOptions {
  // Marked inline so that the chromium style plugin doesn't complain that a
  // "complex constructor" has an inline body. This warning is disabled when
  // the constructor is explicitly marked "inline". Note that this is a false
  // positive of the plugin, since constexpr implies inline.
  inline constexpr PartitionOptions();
  inline constexpr PartitionOptions(const PartitionOptions& other);
  inline constexpr ~PartitionOptions();

  enum class AllowToggle : uint8_t {
    kDisallowed,
    kAllowed,
  };
  enum class EnableToggle : uint8_t {
    kDisabled,
    kEnabled,
  };

  // Expose the enum arms directly at the level of `PartitionOptions`,
  // since the variant names are already sufficiently descriptive.
  static constexpr auto kAllowed = AllowToggle::kAllowed;
  static constexpr auto kDisallowed = AllowToggle::kDisallowed;
  static constexpr auto kDisabled = EnableToggle::kDisabled;
  static constexpr auto kEnabled = EnableToggle::kEnabled;

  EnableToggle thread_cache = kDisabled;
  AllowToggle star_scan_quarantine = kDisallowed;
  EnableToggle backup_ref_ptr = kDisabled;
  AllowToggle use_configurable_pool = kDisallowed;

  EnableToggle scheduler_loop_quarantine = kDisabled;
  size_t scheduler_loop_quarantine_capacity_count = 0;
  size_t scheduler_loop_quarantine_capacity_in_bytes = 0;

  EnableToggle zapping_by_free_flags = kDisabled;

  struct {
    EnableToggle enabled = kDisabled;
    TagViolationReportingMode reporting_mode =
        TagViolationReportingMode::kUndefined;
  } memory_tagging;
#if BUILDFLAG(ENABLE_THREAD_ISOLATION)
  ThreadIsolationOption thread_isolation;
#endif

#if BUILDFLAG(USE_FREELIST_POOL_OFFSETS)
  EnableToggle use_pool_offset_freelists = kDisabled;
#endif
};

constexpr PartitionOptions::PartitionOptions() = default;
constexpr PartitionOptions::PartitionOptions(const PartitionOptions& other) =
    default;
constexpr PartitionOptions::~PartitionOptions() = default;

// When/if free lists should be "straightened" when calling
// PartitionRoot::PurgeMemory(..., accounting_only=false).
enum class StraightenLargerSlotSpanFreeListsMode {
  kNever,
  kOnlyWhenUnprovisioning,
  kAlways,
};

// Never instantiate a PartitionRoot directly, instead use
// PartitionAllocator.
struct PA_ALIGNAS(64) PA_COMPONENT_EXPORT(PARTITION_ALLOC) PartitionRoot {
  using SlotSpanMetadata = internal::SlotSpanMetadata;
  using Bucket = internal::PartitionBucket;
  using FreeListEntry = internal::PartitionFreelistEntry;
  using SuperPageExtentEntry = internal::PartitionSuperPageExtentEntry;
  using DirectMapExtent = internal::PartitionDirectMapExtent;
#if BUILDFLAG(USE_STARSCAN)
  using PCScan = internal::PCScan;
#endif

  enum class QuarantineMode : uint8_t {
    kAlwaysDisabled,
    kDisabledByDefault,
    kEnabled,
  };

  enum class ScanMode : uint8_t {
    kDisabled,
    kEnabled,
  };

  enum class BucketDistribution : uint8_t { kNeutral, kDenser };

  // Root settings accessed on fast paths.
  //
  // Careful! PartitionAlloc's performance is sensitive to its layout.  Please
  // put the fast-path objects in the struct below.
  struct alignas(internal::kPartitionCachelineSize) Settings {
    // Chromium-style: Complex constructor needs an explicit out-of-line
    // constructor.
    Settings();

    // Defines whether objects should be quarantined for this root.
    QuarantineMode quarantine_mode = QuarantineMode::kAlwaysDisabled;

    // Defines whether the root should be scanned.
    ScanMode scan_mode = ScanMode::kDisabled;

    // It's important to default to the 'neutral' distribution, otherwise a
    // switch from 'dense' -> 'neutral' would leave some buckets with dirty
    // memory forever, since no memory would be allocated from these, their
    // freelist would typically not be empty, making these unreclaimable.
    BucketDistribution bucket_distribution = BucketDistribution::kNeutral;

    bool with_thread_cache = false;

#if BUILDFLAG(PA_DCHECK_IS_ON)
    bool use_cookie = false;
#else
    static constexpr bool use_cookie = false;
#endif  // BUILDFLAG(PA_DCHECK_IS_ON)
#if BUILDFLAG(ENABLE_BACKUP_REF_PTR_SUPPORT)
    bool brp_enabled_ = false;
#if PA_CONFIG(MAYBE_ENABLE_MAC11_MALLOC_SIZE_HACK)
    bool mac11_malloc_size_hack_enabled_ = false;
    size_t mac11_malloc_size_hack_usable_size_ = 0;
#endif  // PA_CONFIG(MAYBE_ENABLE_MAC11_MALLOC_SIZE_HACK)
    size_t ref_count_size = 0;
#endif  // BUILDFLAG(ENABLE_BACKUP_REF_PTR_SUPPORT)
    bool use_configurable_pool = false;
    bool zapping_by_free_flags = false;
    bool scheduler_loop_quarantine = false;
#if BUILDFLAG(HAS_MEMORY_TAGGING)
    bool memory_tagging_enabled_ = false;
    TagViolationReportingMode memory_tagging_reporting_mode_ =
        TagViolationReportingMode::kUndefined;
#endif  // BUILDFLAG(HAS_MEMORY_TAGGING)
#if BUILDFLAG(ENABLE_THREAD_ISOLATION)
    ThreadIsolationOption thread_isolation;
#endif

#if BUILDFLAG(USE_FREELIST_POOL_OFFSETS)
    bool use_pool_offset_freelists = false;
#endif

#if PA_CONFIG(EXTRAS_REQUIRED)
    uint32_t extras_size = 0;
#else
    // Teach the compiler that code can be optimized in builds that use no
    // extras.
    static inline constexpr uint32_t extras_size = 0;
#endif  // PA_CONFIG(EXTRAS_REQUIRED)
  };

  Settings settings;

  // Not used on the fastest path (thread cache allocations), but on the fast
  // path of the central allocator.
  alignas(internal::kPartitionCachelineSize) internal::Lock lock_;

  Bucket buckets[internal::kNumBuckets] = {};
  Bucket sentinel_bucket{};

  // All fields below this comment are not accessed on the fast path.
  bool initialized = false;

  // Bookkeeping.
  // - total_size_of_super_pages - total virtual address space for normal bucket
  //     super pages
  // - total_size_of_direct_mapped_pages - total virtual address space for
  //     direct-map regions
  // - total_size_of_committed_pages - total committed pages for slots (doesn't
  //     include metadata, bitmaps (if any), or any data outside or regions
  //     described in #1 and #2)
  // Invariant: total_size_of_allocated_bytes <=
  //            total_size_of_committed_pages <
  //                total_size_of_super_pages +
  //                total_size_of_direct_mapped_pages.
  // Invariant: total_size_of_committed_pages <= max_size_of_committed_pages.
  // Invariant: total_size_of_allocated_bytes <= max_size_of_allocated_bytes.
  // Invariant: max_size_of_allocated_bytes <= max_size_of_committed_pages.
  // Since all operations on the atomic variables have relaxed semantics, we
  // don't check these invariants with DCHECKs.
  std::atomic<size_t> total_size_of_committed_pages{0};
  std::atomic<size_t> max_size_of_committed_pages{0};
  std::atomic<size_t> total_size_of_super_pages{0};
  std::atomic<size_t> total_size_of_direct_mapped_pages{0};
  size_t total_size_of_allocated_bytes
      PA_GUARDED_BY(internal::PartitionRootLock(this)) = 0;
  size_t max_size_of_allocated_bytes
      PA_GUARDED_BY(internal::PartitionRootLock(this)) = 0;
  // Atomic, because system calls can be made without the lock held.
  std::atomic<uint64_t> syscall_count{};
  std::atomic<uint64_t> syscall_total_time_ns{};
#if BUILDFLAG(ENABLE_BACKUP_REF_PTR_SUPPORT)
  std::atomic<size_t> total_size_of_brp_quarantined_bytes{0};
  std::atomic<size_t> total_count_of_brp_quarantined_slots{0};
  std::atomic<size_t> cumulative_size_of_brp_quarantined_bytes{0};
  std::atomic<size_t> cumulative_count_of_brp_quarantined_slots{0};
#endif
  // Slot span memory which has been provisioned, and is currently unused as
  // it's part of an empty SlotSpan. This is not clean memory, since it has
  // either been used for a memory allocation, and/or contains freelist
  // entries. But it might have been moved to swap. Note that all this memory
  // can be decommitted at any time.
  size_t empty_slot_spans_dirty_bytes
      PA_GUARDED_BY(internal::PartitionRootLock(this)) = 0;

  // Only tolerate up to |total_size_of_committed_pages >>
  // max_empty_slot_spans_dirty_bytes_shift| dirty bytes in empty slot
  // spans. That is, the default value of 3 tolerates up to 1/8. Since
  // |empty_slot_spans_dirty_bytes| is never strictly larger than
  // total_size_of_committed_pages, setting this to 0 removes the cap. This is
  // useful to make tests deterministic and easier to reason about.
  int max_empty_slot_spans_dirty_bytes_shift = 3;

  uintptr_t next_super_page = 0;
  uintptr_t next_partition_page = 0;
  uintptr_t next_partition_page_end = 0;
  SuperPageExtentEntry* current_extent = nullptr;
  SuperPageExtentEntry* first_extent = nullptr;
  DirectMapExtent* direct_map_list
      PA_GUARDED_BY(internal::PartitionRootLock(this)) = nullptr;
  SlotSpanMetadata*
      global_empty_slot_span_ring[internal::kMaxFreeableSpans] PA_GUARDED_BY(
          internal::PartitionRootLock(this)) = {};
  int16_t global_empty_slot_span_ring_index
      PA_GUARDED_BY(internal::PartitionRootLock(this)) = 0;
  int16_t global_empty_slot_span_ring_size
      PA_GUARDED_BY(internal::PartitionRootLock(this)) =
          internal::kDefaultEmptySlotSpanRingSize;

  // Integrity check = ~reinterpret_cast<uintptr_t>(this).
  uintptr_t inverted_self = 0;
  std::atomic<int> thread_caches_being_constructed_{0};

  bool quarantine_always_for_testing = false;

  size_t scheduler_loop_quarantine_capacity_count = 0;
  size_t scheduler_loop_quarantine_capacity_in_bytes = 0;
  internal::LightweightQuarantineRoot scheduler_loop_quarantine_root;
  // NoDestructor because we don't need to dequarantine objects as the root
  // associated with it is dying anyway.
  std::optional<
      internal::base::NoDestructor<internal::LightweightQuarantineBranch>>
      scheduler_loop_quarantine;

  PartitionRoot();
  explicit PartitionRoot(PartitionOptions opts);

  // TODO(tasak): remove ~PartitionRoot() after confirming all tests
  // don't need ~PartitionRoot().
  ~PartitionRoot();

  // This will unreserve any space in the pool that the PartitionRoot is
  // using. This is needed because many tests create and destroy many
  // PartitionRoots over the lifetime of a process, which can exhaust the
  // pool and cause tests to fail.
  void DestructForTesting();

#if PA_CONFIG(MAYBE_ENABLE_MAC11_MALLOC_SIZE_HACK)
  void EnableMac11MallocSizeHackIfNeeded();
  void EnableMac11MallocSizeHackForTesting();
  void InitMac11MallocSizeHackUsableSize();
#endif  // PA_CONFIG(MAYBE_ENABLE_MAC11_MALLOC_SIZE_HACK)

  // Public API
  //
  // Allocates out of the given bucket. Properly, this function should probably
  // be in PartitionBucket, but because the implementation needs to be inlined
  // for performance, and because it needs to inspect SlotSpanMetadata,
  // it becomes impossible to have it in PartitionBucket as this causes a
  // cyclical dependency on SlotSpanMetadata function implementations.
  //
  // Moving it a layer lower couples PartitionRoot and PartitionBucket, but
  // preserves the layering of the includes.
  void Init(PartitionOptions);

  void EnableThreadCacheIfSupported();

  PA_ALWAYS_INLINE static PartitionRoot* FromSlotSpanMetadata(
      SlotSpanMetadata* slot_span);
  // These two functions work unconditionally for normal buckets.
  // For direct map, they only work for the first super page of a reservation,
  // (see partition_alloc_constants.h for the direct map allocation layout).
  // In particular, the functions always work for a pointer to the start of a
  // reservation.
  PA_ALWAYS_INLINE static PartitionRoot* FromFirstSuperPage(
      uintptr_t super_page);
  PA_ALWAYS_INLINE static PartitionRoot* FromAddrInFirstSuperpage(
      uintptr_t address);

  PA_ALWAYS_INLINE void DecreaseTotalSizeOfAllocatedBytes(uintptr_t addr,
                                                          size_t len)
      PA_EXCLUSIVE_LOCKS_REQUIRED(internal::PartitionRootLock(this));
  PA_ALWAYS_INLINE void IncreaseTotalSizeOfAllocatedBytes(uintptr_t addr,
                                                          size_t len,
                                                          size_t raw_size)
      PA_EXCLUSIVE_LOCKS_REQUIRED(internal::PartitionRootLock(this));
  PA_ALWAYS_INLINE void IncreaseCommittedPages(size_t len);
  PA_ALWAYS_INLINE void DecreaseCommittedPages(size_t len);
  PA_ALWAYS_INLINE void DecommitSystemPagesForData(
      uintptr_t address,
      size_t length,
      PageAccessibilityDisposition accessibility_disposition)
      PA_EXCLUSIVE_LOCKS_REQUIRED(internal::PartitionRootLock(this));
  PA_ALWAYS_INLINE void RecommitSystemPagesForData(
      uintptr_t address,
      size_t length,
      PageAccessibilityDisposition accessibility_disposition,
      bool request_tagging)
      PA_EXCLUSIVE_LOCKS_REQUIRED(internal::PartitionRootLock(this));

  template <bool already_locked>
  PA_ALWAYS_INLINE bool TryRecommitSystemPagesForDataInternal(
      uintptr_t address,
      size_t length,
      PageAccessibilityDisposition accessibility_disposition,
      bool request_tagging);

  // TryRecommitSystemPagesForDataWithAcquiringLock() locks this root internally
  // before invoking DecommitEmptySlotSpans(), which needs the lock. So the root
  // must not be locked when invoking this method.
  PA_ALWAYS_INLINE bool TryRecommitSystemPagesForDataWithAcquiringLock(
      uintptr_t address,
      size_t length,
      PageAccessibilityDisposition accessibility_disposition,
      bool request_tagging)
      PA_LOCKS_EXCLUDED(internal::PartitionRootLock(this));

  // TryRecommitSystemPagesForDataLocked() doesn't lock this root internally
  // before invoking DecommitEmptySlotSpans(), which needs the lock. So the root
  // must have been already locked when invoking this method.
  PA_ALWAYS_INLINE bool TryRecommitSystemPagesForDataLocked(
      uintptr_t address,
      size_t length,
      PageAccessibilityDisposition accessibility_disposition,
      bool request_tagging)
      PA_EXCLUSIVE_LOCKS_REQUIRED(internal::PartitionRootLock(this));

  [[noreturn]] PA_NOINLINE void OutOfMemory(size_t size);

  // Returns a pointer aligned on |alignment|, or nullptr.
  //
  // |alignment| has to be a power of two and a multiple of sizeof(void*) (as in
  // posix_memalign() for POSIX systems). The returned pointer may include
  // padding, and can be passed to |Free()| later.
  //
  // NOTE: This is incompatible with anything that adds extras before the
  // returned pointer, such as ref-count.
  template <AllocFlags flags = AllocFlags::kNone>
  PA_NOINLINE void* AlignedAlloc(size_t alignment, size_t requested_size) {
    return AlignedAllocInline<flags>(alignment, requested_size);
  }
  template <AllocFlags flags = AllocFlags::kNone>
  PA_ALWAYS_INLINE void* AlignedAllocInline(size_t alignment,
                                            size_t requested_size);

  // PartitionAlloc supports multiple partitions, and hence multiple callers to
  // these functions. Setting PA_ALWAYS_INLINE bloats code, and can be
  // detrimental to performance, for instance if multiple callers are hot (by
  // increasing cache footprint). Set PA_NOINLINE on the "basic" top-level
  // functions to mitigate that for "vanilla" callers.
  //
  // |type_name == nullptr|: ONLY FOR TESTS except internal uses.
  // You should provide |type_name| to make debugging easier.
  template <AllocFlags flags = AllocFlags::kNone>
  PA_NOINLINE PA_MALLOC_FN PA_MALLOC_ALIGNED void* Alloc(
      size_t requested_size,
      const char* type_name = nullptr) {
    return AllocInline<flags>(requested_size, type_name);
  }
  template <AllocFlags flags = AllocFlags::kNone>
  PA_ALWAYS_INLINE PA_MALLOC_FN PA_MALLOC_ALIGNED void* AllocInline(
      size_t requested_size,
      const char* type_name = nullptr) {
    return AllocInternal<flags>(requested_size, internal::PartitionPageSize(),
                                type_name);
  }

  // AllocInternal exposed for testing.
  template <AllocFlags flags = AllocFlags::kNone>
  PA_NOINLINE PA_MALLOC_FN PA_MALLOC_ALIGNED void* AllocInternalForTesting(
      size_t requested_size,
      size_t slot_span_alignment,
      const char* type_name) {
    return AllocInternal<flags>(requested_size, slot_span_alignment, type_name);
  }

  template <AllocFlags alloc_flags = AllocFlags::kNone,
            FreeFlags free_flags = FreeFlags::kNone>
  PA_NOINLINE PA_MALLOC_ALIGNED void* Realloc(void* ptr,
                                              size_t new_size,
                                              const char* type_name) {
    return ReallocInline<alloc_flags, free_flags>(ptr, new_size, type_name);
  }
  template <AllocFlags alloc_flags = AllocFlags::kNone,
            FreeFlags free_flags = FreeFlags::kNone>
  PA_ALWAYS_INLINE PA_MALLOC_ALIGNED void* ReallocInline(void* ptr,
                                                         size_t new_size,
                                                         const char* type_name);

  template <FreeFlags flags = FreeFlags::kNone>
  PA_NOINLINE void Free(void* object) {
    FreeInline<flags>(object);
  }
  template <FreeFlags flags = FreeFlags::kNone>
  PA_ALWAYS_INLINE void FreeInline(void* object);

  template <FreeFlags flags = FreeFlags::kNone>
  PA_NOINLINE static void FreeInUnknownRoot(void* object) {
    FreeInlineInUnknownRoot<flags>(object);
  }
  template <FreeFlags flags = FreeFlags::kNone>
  PA_ALWAYS_INLINE static void FreeInlineInUnknownRoot(void* object);

  // Immediately frees the pointer bypassing the quarantine. |slot_start| is the
  // beginning of the slot that contains |object|.
  PA_ALWAYS_INLINE void FreeNoHooksImmediate(void* object,
                                             SlotSpanMetadata* slot_span,
                                             uintptr_t slot_start);

  PA_ALWAYS_INLINE size_t GetSlotUsableSize(SlotSpanMetadata* slot_span) {
    return AdjustSizeForExtrasSubtract(slot_span->GetUtilizedSlotSize());
  }

  PA_ALWAYS_INLINE static size_t GetUsableSize(void* ptr);

  // Same as GetUsableSize() except it adjusts the return value for macOS 11
  // malloc_size() hack.
  PA_ALWAYS_INLINE static size_t GetUsableSizeWithMac11MallocSizeHack(
      void* ptr);

  PA_ALWAYS_INLINE PageAccessibilityConfiguration
  GetPageAccessibility(bool request_tagging) const;
  PA_ALWAYS_INLINE PageAccessibilityConfiguration
      PageAccessibilityWithThreadIsolationIfEnabled(
          PageAccessibilityConfiguration::Permissions) const;

  PA_ALWAYS_INLINE size_t
  AllocationCapacityFromSlotStart(uintptr_t slot_start) const;
  PA_ALWAYS_INLINE size_t
  AllocationCapacityFromRequestedSize(size_t size) const;

#if BUILDFLAG(ENABLE_BACKUP_REF_PTR_SUPPORT)
  PA_ALWAYS_INLINE static internal::PartitionRefCount*
  RefCountPointerFromSlotStartAndSize(uintptr_t slot_start, size_t slot_size);
  PA_ALWAYS_INLINE internal::PartitionRefCount*
  RefCountPointerFromObjectForTesting(void* object) const;
#endif  // BUILDFLAG(ENABLE_BACKUP_REF_PTR_SUPPORT)

  PA_ALWAYS_INLINE bool IsMemoryTaggingEnabled() const;
  PA_ALWAYS_INLINE TagViolationReportingMode
  memory_tagging_reporting_mode() const;

  // Frees memory from this partition, if possible, by decommitting pages or
  // even entire slot spans. |flags| is an OR of base::PartitionPurgeFlags.
  void PurgeMemory(int flags);

  // Reduces the size of the empty slot spans ring, until the dirty size is <=
  // |limit|.
  void ShrinkEmptySlotSpansRing(size_t limit)
      PA_EXCLUSIVE_LOCKS_REQUIRED(internal::PartitionRootLock(this));
  // The empty slot span ring starts "small", can be enlarged later. This
  // improves performance by performing fewer system calls, at the cost of more
  // memory usage.
  void EnableLargeEmptySlotSpanRing() {
    ::partition_alloc::internal::ScopedGuard locker{
        internal::PartitionRootLock(this)};
    global_empty_slot_span_ring_size = internal::kMaxFreeableSpans;
  }

  void DumpStats(const char* partition_name,
                 bool is_light_dump,
                 PartitionStatsDumper* partition_stats_dumper);

  static void DeleteForTesting(PartitionRoot* partition_root);
  void ResetForTesting(bool allow_leaks);
  void ResetBookkeepingForTesting();

  PA_ALWAYS_INLINE BucketDistribution GetBucketDistribution() const {
    return settings.bucket_distribution;
  }

  static uint16_t SizeToBucketIndex(size_t size,
                                    BucketDistribution bucket_distribution);

  PA_ALWAYS_INLINE void FreeInSlotSpan(uintptr_t slot_start,
                                       SlotSpanMetadata* slot_span)
      PA_EXCLUSIVE_LOCKS_REQUIRED(internal::PartitionRootLock(this));

  // Frees memory, with |slot_start| as returned by |RawAlloc()|.
  PA_ALWAYS_INLINE void RawFree(uintptr_t slot_start);
  PA_ALWAYS_INLINE void RawFree(uintptr_t slot_start,
                                SlotSpanMetadata* slot_span)
      PA_LOCKS_EXCLUDED(internal::PartitionRootLock(this));

  PA_ALWAYS_INLINE void RawFreeBatch(FreeListEntry* head,
                                     FreeListEntry* tail,
                                     size_t size,
                                     SlotSpanMetadata* slot_span)
      PA_LOCKS_EXCLUDED(internal::PartitionRootLock(this));

  PA_ALWAYS_INLINE void RawFreeWithThreadCache(uintptr_t slot_start,
                                               SlotSpanMetadata* slot_span);

  // This is safe to do because we are switching to a bucket distribution with
  // more buckets, meaning any allocations we have done before the switch are
  // guaranteed to have a bucket under the new distribution when they are
  // eventually deallocated. We do not need synchronization here.
  void SwitchToDenserBucketDistribution() {
    settings.bucket_distribution = BucketDistribution::kDenser;
  }
  // Switching back to the less dense bucket distribution is ok during tests.
  // At worst, we end up with deallocations that are sent to a bucket that we
  // cannot allocate from, which will not cause problems besides wasting
  // memory.
  void ResetBucketDistributionForTesting() {
    settings.bucket_distribution = BucketDistribution::kNeutral;
  }

  ThreadCache* thread_cache_for_testing() const {
    return settings.with_thread_cache ? ThreadCache::Get() : nullptr;
  }
  size_t get_total_size_of_committed_pages() const {
    return total_size_of_committed_pages.load(std::memory_order_relaxed);
  }
  size_t get_max_size_of_committed_pages() const {
    return max_size_of_committed_pages.load(std::memory_order_relaxed);
  }

  size_t get_total_size_of_allocated_bytes() const {
    // Since this is only used for bookkeeping, we don't care if the value is
    // stale, so no need to get a lock here.
    return PA_TS_UNCHECKED_READ(total_size_of_allocated_bytes);
  }

  size_t get_max_size_of_allocated_bytes() const {
    // Since this is only used for bookkeeping, we don't care if the value is
    // stale, so no need to get a lock here.
    return PA_TS_UNCHECKED_READ(max_size_of_allocated_bytes);
  }

  internal::pool_handle ChoosePool() const {
#if BUILDFLAG(HAS_64_BIT_POINTERS)
    if (settings.use_configurable_pool) {
      PA_DCHECK(IsConfigurablePoolAvailable());
      return internal::kConfigurablePoolHandle;
    }
#endif
#if BUILDFLAG(ENABLE_THREAD_ISOLATION)
    if (settings.thread_isolation.enabled) {
      return internal::kThreadIsolatedPoolHandle;
    }
#endif
#if BUILDFLAG(ENABLE_BACKUP_REF_PTR_SUPPORT)
    return brp_enabled() ? internal::kBRPPoolHandle
                         : internal::kRegularPoolHandle;
#else
    return internal::kRegularPoolHandle;
#endif  // BUILDFLAG(ENABLE_BACKUP_REF_PTR_SUPPORT)
  }

  PA_ALWAYS_INLINE bool IsQuarantineAllowed() const {
    return settings.quarantine_mode != QuarantineMode::kAlwaysDisabled;
  }

  PA_ALWAYS_INLINE bool IsQuarantineEnabled() const {
    return settings.quarantine_mode == QuarantineMode::kEnabled;
  }

  PA_ALWAYS_INLINE bool ShouldQuarantine(void* object) const {
    if (PA_UNLIKELY(settings.quarantine_mode != QuarantineMode::kEnabled)) {
      return false;
    }
#if BUILDFLAG(HAS_MEMORY_TAGGING)
    if (PA_UNLIKELY(quarantine_always_for_testing)) {
      return true;
    }
    // If quarantine is enabled and the tag overflows, move the containing slot
    // to quarantine, to prevent the attacker from exploiting a pointer that has
    // an old tag.
    if (PA_LIKELY(IsMemoryTaggingEnabled())) {
      return internal::HasOverflowTag(object);
    }
    // Default behaviour if MTE is not enabled for this PartitionRoot.
    return true;
#else
    return true;
#endif  // BUILDFLAG(HAS_MEMORY_TAGGING)
  }

  PA_ALWAYS_INLINE void SetQuarantineAlwaysForTesting(bool value) {
    quarantine_always_for_testing = value;
  }

  PA_ALWAYS_INLINE bool IsScanEnabled() const {
    // Enabled scan implies enabled quarantine.
    PA_DCHECK(settings.scan_mode != ScanMode::kEnabled ||
              IsQuarantineEnabled());
    return settings.scan_mode == ScanMode::kEnabled;
  }

  PA_ALWAYS_INLINE static PAGE_ALLOCATOR_CONSTANTS_DECLARE_CONSTEXPR size_t
  GetDirectMapMetadataAndGuardPagesSize() {
    // Because we need to fake a direct-map region to look like a super page, we
    // need to allocate more pages around the payload:
    // - The first partition page is a combination of metadata and guard region.
    // - We also add a trailing guard page. In most cases, a system page would
    //   suffice. But on 32-bit systems when BRP is on, we need a partition page
    //   to match granularity of the BRP pool bitmap. For cosistency, we'll use
    //   a partition page everywhere, which is cheap as it's uncommitted address
    //   space anyway.
    return 2 * internal::PartitionPageSize();
  }

  PA_ALWAYS_INLINE static PAGE_ALLOCATOR_CONSTANTS_DECLARE_CONSTEXPR size_t
  GetDirectMapSlotSize(size_t raw_size) {
    // Caller must check that the size is not above the MaxDirectMapped()
    // limit before calling. This also guards against integer overflow in the
    // calculation here.
    PA_DCHECK(raw_size <= internal::MaxDirectMapped());
    return partition_alloc::internal::base::bits::AlignUp(
        raw_size, internal::SystemPageSize());
  }

  PA_ALWAYS_INLINE static size_t GetDirectMapReservationSize(
      size_t padded_raw_size) {
    // Caller must check that the size is not above the MaxDirectMapped()
    // limit before calling. This also guards against integer overflow in the
    // calculation here.
    PA_DCHECK(padded_raw_size <= internal::MaxDirectMapped());
    return partition_alloc::internal::base::bits::AlignUp(
        padded_raw_size + GetDirectMapMetadataAndGuardPagesSize(),
        internal::DirectMapAllocationGranularity());
  }

  PA_ALWAYS_INLINE size_t AdjustSize0IfNeeded(size_t size) const {
    // There are known cases where allowing size 0 would lead to problems:
    // 1. If extras are present only before allocation (e.g. BRP ref-count), the
    //    extras will fill the entire kAlignment-sized slot, leading to
    //    returning a pointer to the next slot. Realloc() calls
    //    SlotSpanMetadata::FromObject() prior to subtracting extras, thus
    //    potentially getting a wrong slot span.
    // 2. If we put BRP ref-count in the previous slot, that slot may be free.
    //    In this case, the slot needs to fit both, a free-list entry and a
    //    ref-count. If sizeof(PartitionRefCount) is 8, it fills the entire
    //    smallest slot on 32-bit systems (kSmallestBucket is 8), thus not
    //    leaving space for the free-list entry.
    // 3. On macOS and iOS, PartitionGetSizeEstimate() is used for two purposes:
    //    as a zone dispatcher and as an underlying implementation of
    //    malloc_size(3). As a zone dispatcher, zero has a special meaning of
    //    "doesn't belong to this zone". When extras fill out the entire slot,
    //    the usable size is 0, thus confusing the zone dispatcher.
    //
    // To save ourselves a branch on this hot path, we could eliminate this
    // check at compile time for cases not listed above. The #if statement would
    // be rather complex. Then there is also the fear of the unknown. The
    // existing cases were discovered through obscure, painful-to-debug crashes.
    // Better save ourselves trouble with not-yet-discovered cases.
    if (PA_UNLIKELY(size == 0)) {
      return 1;
    }
    return size;
  }

  // Adjusts the size by adding extras. Also include the 0->1 adjustment if
  // needed.
  PA_ALWAYS_INLINE size_t AdjustSizeForExtrasAdd(size_t size) const {
    size = AdjustSize0IfNeeded(size);
    PA_DCHECK(size + settings.extras_size >= size);
    return size + settings.extras_size;
  }

  // Adjusts the size by subtracing extras. Doesn't include the 0->1 adjustment,
  // which leads to an asymmetry with AdjustSizeForExtrasAdd, but callers of
  // AdjustSizeForExtrasSubtract either expect the adjustment to be included, or
  // are indifferent.
  PA_ALWAYS_INLINE size_t AdjustSizeForExtrasSubtract(size_t size) const {
    return size - settings.extras_size;
  }

  PA_ALWAYS_INLINE uintptr_t SlotStartToObjectAddr(uintptr_t slot_start) const {
    // TODO(bartekn): Check that |slot_start| is indeed a slot start.
    return slot_start;
  }

  PA_ALWAYS_INLINE void* SlotStartToObject(uintptr_t slot_start) const {
    // TODO(bartekn): Check that |slot_start| is indeed a slot start.
    return internal::TagAddr(SlotStartToObjectAddr(slot_start));
  }

  PA_ALWAYS_INLINE void* TaggedSlotStartToObject(
      void* tagged_slot_start) const {
    // TODO(bartekn): Check that |tagged_slot_start| is indeed a slot start.
    return reinterpret_cast<void*>(
        SlotStartToObjectAddr(reinterpret_cast<uintptr_t>(tagged_slot_start)));
  }

  PA_ALWAYS_INLINE uintptr_t ObjectToSlotStart(void* object) const {
    return UntagPtr(object);
    // TODO(bartekn): Check that the result is indeed a slot start.
  }

  PA_ALWAYS_INLINE uintptr_t ObjectToTaggedSlotStart(void* object) const {
    return reinterpret_cast<uintptr_t>(object);
    // TODO(bartekn): Check that the result is indeed a slot start.
  }

  bool brp_enabled() const {
#if BUILDFLAG(ENABLE_BACKUP_REF_PTR_SUPPORT)
    return settings.brp_enabled_;
#else
    return false;
#endif
  }

  PA_ALWAYS_INLINE bool uses_configurable_pool() const {
    return settings.use_configurable_pool;
  }

  // To make tests deterministic, it is necessary to uncap the amount of memory
  // waste incurred by empty slot spans. Otherwise, the size of various
  // freelists, and committed memory becomes harder to reason about (and
  // brittle) with a single thread, and non-deterministic with several.
  void UncapEmptySlotSpanMemoryForTesting() {
    max_empty_slot_spans_dirty_bytes_shift = 0;
  }

  // Enables/disables the free list straightening for larger slot spans in
  // PurgeMemory().
  static void SetStraightenLargerSlotSpanFreeListsMode(
      StraightenLargerSlotSpanFreeListsMode new_value);
  // Enables/disables the free list sorting for smaller slot spans in
  // PurgeMemory().
  static void SetSortSmallerSlotSpanFreeListsEnabled(bool new_value);
  // Enables/disables the sorting of active slot spans in PurgeMemory().
  static void SetSortActiveSlotSpansEnabled(bool new_value);

  static StraightenLargerSlotSpanFreeListsMode
  GetStraightenLargerSlotSpanFreeListsMode() {
    return straighten_larger_slot_span_free_lists_;
  }

  internal::LightweightQuarantineBranch&
  GetSchedulerLoopQuarantineBranchForTesting() {
    return GetSchedulerLoopQuarantineBranch();
  }

#if BUILDFLAG(USE_FREELIST_POOL_OFFSETS)
  PA_ALWAYS_INLINE bool uses_pool_offset_freelists() const {
    return settings.use_pool_offset_freelists;
  }
#endif  // BUILDFLAG(USE_FREELIST_POOL_OFFSETS)

#if BUILDFLAG(HAS_MEMORY_TAGGING)
  // Returns size that should be tagged. Avoiding the previous slot ref count if
  // it exists to avoid a race (crbug.com/1445816).
  PA_ALWAYS_INLINE size_t TagSizeForSlot(size_t slot_size) {
#if PA_CONFIG(MAYBE_INCREASE_REF_COUNT_SIZE_FOR_MTE)
#if BUILDFLAG(PA_DCHECK_IS_ON)
    if (brp_enabled()) {
      PA_DCHECK(settings.ref_count_size > 0);
      PA_DCHECK((settings.ref_count_size % internal::kMemTagGranuleSize) == 0);
    } else {
      PA_DCHECK(settings.ref_count_size == 0);
    }
#endif  // BUILDFLAG(PA_DCHECK_IS_ON)
    // TODO(bartekn): Don't subtract ref-count size in the "same slot" mode.
    return slot_size - settings.ref_count_size;
#else  // PA_CONFIG(MAYBE_INCREASE_REF_COUNT_SIZE_FOR_MTE)
    return slot_size;
#endif
  }
#endif  // BUILDFLAG(HAS_MEMORY_TAGGING)

  static void SetBrpRefCountInSameSlot(bool ref_count_in_same_slot) {
#if BUILDFLAG(ENABLE_BACKUP_REF_PTR_SUPPORT)
    ref_count_in_same_slot_ = ref_count_in_same_slot;
#endif
  }

 private:
  static inline StraightenLargerSlotSpanFreeListsMode
      straighten_larger_slot_span_free_lists_ =
          StraightenLargerSlotSpanFreeListsMode::kOnlyWhenUnprovisioning;
  static inline bool sort_smaller_slot_span_free_lists_ = true;
  static inline bool sort_active_slot_spans_ = false;
#if BUILDFLAG(ENABLE_BACKUP_REF_PTR_SUPPORT)
  static inline bool ref_count_in_same_slot_ = false;
#endif

  // Common path of Free() and FreeInUnknownRoot(). Returns
  // true if the caller should return immediately.
  template <FreeFlags flags>
  PA_ALWAYS_INLINE static bool FreeProlog(void* object,
                                          const PartitionRoot* root);

  // |buckets| has `kNumBuckets` elements, but we sometimes access it at index
  // `kNumBuckets`, which is occupied by the sentinel bucket. The correct layout
  // is enforced by a static_assert() in partition_root.cc, so this is
  // fine. However, UBSAN is correctly pointing out that there is an
  // out-of-bounds access, so disable it for these accesses.
  //
  // See crbug.com/1150772 for an instance of Clusterfuzz / UBSAN detecting
  // this.
  PA_ALWAYS_INLINE const Bucket& PA_NO_SANITIZE("undefined")
      bucket_at(size_t i) const {
    PA_DCHECK(i <= internal::kNumBuckets);
    return buckets[i];
  }

  // Returns whether a |bucket| from |this| root is direct-mapped. This function
  // does not touch |bucket|, contrary to  PartitionBucket::is_direct_mapped().
  //
  // This is meant to be used in hot paths, and particularly *before* going into
  // the thread cache fast path. Indeed, real-world profiles show that accessing
  // an allocation's bucket is responsible for a sizable fraction of *total*
  // deallocation time. This can be understood because
  // - All deallocations have to access the bucket to know whether it is
  //   direct-mapped. If not (vast majority of allocations), it can go through
  //   the fast path, i.e. thread cache.
  // - The bucket is relatively frequently written to, by *all* threads
  //   (e.g. every time a slot span becomes full or empty), so accessing it will
  //   result in some amount of cacheline ping-pong.
  PA_ALWAYS_INLINE bool IsDirectMappedBucket(Bucket* bucket) const {
    // All regular allocations are associated with a bucket in the |buckets_|
    // array. A range check is then sufficient to identify direct-mapped
    // allocations.
    bool ret = !(bucket >= this->buckets && bucket <= &this->sentinel_bucket);
    PA_DCHECK(ret == bucket->is_direct_mapped());
    return ret;
  }

  // Same as |Alloc()|, but allows specifying |slot_span_alignment|. It
  // has to be a multiple of partition page size, greater than 0 and no greater
  // than kMaxSupportedAlignment. If it equals exactly 1 partition page, no
  // special action is taken as PartitionAlloc naturally guarantees this
  // alignment, otherwise a sub-optimal allocation strategy is used to
  // guarantee the higher-order alignment.
  template <AllocFlags flags>
  PA_ALWAYS_INLINE PA_MALLOC_FN PA_MALLOC_ALIGNED void* AllocInternal(
      size_t requested_size,
      size_t slot_span_alignment,
      const char* type_name);

  // Same as |AllocInternal()|, but don't handle allocation hooks.
  template <AllocFlags flags = AllocFlags::kNone>
  PA_ALWAYS_INLINE PA_MALLOC_FN PA_MALLOC_ALIGNED void* AllocInternalNoHooks(
      size_t requested_size,
      size_t slot_span_alignment);
  // Allocates a memory slot, without initializing extras.
  //
  // - |flags| are as in Alloc().
  // - |raw_size| accommodates for extras on top of Alloc()'s
  //   |requested_size|.
  // - |usable_size|, |slot_size| and |is_already_zeroed| are output only.
  //   Note, |usable_size| is guaranteed to be no smaller than Alloc()'s
  //   |requested_size|, and no larger than |slot_size|.
  template <AllocFlags flags>
  PA_ALWAYS_INLINE uintptr_t RawAlloc(Bucket* bucket,
                                      size_t raw_size,
                                      size_t slot_span_alignment,
                                      size_t* usable_size,
                                      size_t* slot_size,
                                      bool* is_already_zeroed);
  template <AllocFlags flags>
  PA_ALWAYS_INLINE uintptr_t AllocFromBucket(Bucket* bucket,
                                             size_t raw_size,
                                             size_t slot_span_alignment,
                                             size_t* usable_size,
                                             size_t* slot_size,
                                             bool* is_already_zeroed)
      PA_EXCLUSIVE_LOCKS_REQUIRED(internal::PartitionRootLock(this));

  // We use this to make MEMORY_TOOL_REPLACES_ALLOCATOR behave the same for max
  // size as other alloc code.
  template <AllocFlags flags>
  PA_ALWAYS_INLINE static bool AllocWithMemoryToolProlog(size_t size) {
    if (size > partition_alloc::internal::MaxDirectMapped()) {
      if constexpr (ContainsFlags(flags, AllocFlags::kReturnNull)) {
        // Early return indicating not to proceed with allocation
        return false;
      }
      PA_CHECK(false);
    }
    return true;  // Allocation should proceed
  }

  bool TryReallocInPlaceForNormalBuckets(void* object,
                                         SlotSpanMetadata* slot_span,
                                         size_t new_size);
  bool TryReallocInPlaceForDirectMap(internal::SlotSpanMetadata* slot_span,
                                     size_t requested_size)
      PA_EXCLUSIVE_LOCKS_REQUIRED(internal::PartitionRootLock(this));
  void DecommitEmptySlotSpans()
      PA_EXCLUSIVE_LOCKS_REQUIRED(internal::PartitionRootLock(this));
  PA_ALWAYS_INLINE void RawFreeLocked(uintptr_t slot_start)
      PA_EXCLUSIVE_LOCKS_REQUIRED(internal::PartitionRootLock(this));
  ThreadCache* MaybeInitThreadCache();

  // May return an invalid thread cache.
  PA_ALWAYS_INLINE ThreadCache* GetOrCreateThreadCache();
  PA_ALWAYS_INLINE ThreadCache* GetThreadCache();

  PA_ALWAYS_INLINE internal::LightweightQuarantineBranch&
  GetSchedulerLoopQuarantineBranch();

  internal::LightweightQuarantineBranch CreateSchedulerLoopQuarantineBranch(
      bool lock_required);

  PA_ALWAYS_INLINE AllocationNotificationData
  CreateAllocationNotificationData(void* object,
                                   size_t size,
                                   const char* type_name) const;
  PA_ALWAYS_INLINE static FreeNotificationData
  CreateDefaultFreeNotificationData(void* address);
  PA_ALWAYS_INLINE FreeNotificationData
  CreateFreeNotificationData(void* address) const;

#if PA_CONFIG(USE_PARTITION_ROOT_ENUMERATOR)
  static internal::Lock& GetEnumeratorLock();

  PartitionRoot* PA_GUARDED_BY(GetEnumeratorLock()) next_root = nullptr;
  PartitionRoot* PA_GUARDED_BY(GetEnumeratorLock()) prev_root = nullptr;

  friend class internal::PartitionRootEnumerator;
#endif  // PA_CONFIG(USE_PARTITION_ROOT_ENUMERATOR)

  friend class ThreadCache;
};

namespace internal {

PA_ALWAYS_INLINE ::partition_alloc::internal::Lock& PartitionRootLock(
    PartitionRoot* root) {
  return root->lock_;
}

class ScopedSyscallTimer {
 public:
#if PA_CONFIG(COUNT_SYSCALL_TIME)
  explicit ScopedSyscallTimer(PartitionRoot* root)
      : root_(root), tick_(base::TimeTicks::Now()) {}

  ~ScopedSyscallTimer() {
    root_->syscall_count.fetch_add(1, std::memory_order_relaxed);

    int64_t elapsed_nanos = (base::TimeTicks::Now() - tick_).InNanoseconds();
    if (elapsed_nanos > 0) {
      root_->syscall_total_time_ns.fetch_add(
          static_cast<uint64_t>(elapsed_nanos), std::memory_order_relaxed);
    }
  }

 private:
  PartitionRoot* root_;
  const base::TimeTicks tick_;
#else
  explicit ScopedSyscallTimer(PartitionRoot* root) {
    root->syscall_count.fetch_add(1, std::memory_order_relaxed);
  }
#endif
};

#if BUILDFLAG(ENABLE_BACKUP_REF_PTR_SUPPORT)

PA_ALWAYS_INLINE std::pair<uintptr_t, size_t>
PartitionAllocGetDirectMapSlotStartAndSizeInBRPPool(uintptr_t address) {
  PA_DCHECK(IsManagedByPartitionAllocBRPPool(address));
#if BUILDFLAG(HAS_64_BIT_POINTERS)
  // Use this variant of GetDirectMapReservationStart as it has better
  // performance.
  uintptr_t offset = OffsetInBRPPool(address);
  uintptr_t reservation_start =
      GetDirectMapReservationStart(address, kBRPPoolHandle, offset);
#else  // BUILDFLAG(HAS_64_BIT_POINTERS)
  uintptr_t reservation_start = GetDirectMapReservationStart(address);
#endif
  if (!reservation_start) {
    return std::make_pair(uintptr_t(0), size_t(0));
  }

  // The direct map allocation may not start exactly from the first page, as
  // there may be padding for alignment. The first page metadata holds an offset
  // to where direct map metadata, and thus direct map start, are located.
  auto* first_page_metadata =
      PartitionPageMetadata::FromAddr(reservation_start + PartitionPageSize());
  auto* page_metadata =
      first_page_metadata + first_page_metadata->slot_span_metadata_offset;
  PA_DCHECK(page_metadata->is_valid);
  PA_DCHECK(!page_metadata->slot_span_metadata_offset);
  auto* slot_span = &page_metadata->slot_span_metadata;
  uintptr_t slot_start = SlotSpanMetadata::ToSlotSpanStart(slot_span);
#if BUILDFLAG(PA_DCHECK_IS_ON)
  auto* direct_map_metadata =
      PartitionDirectMapMetadata::FromSlotSpanMetadata(slot_span);
  size_t padding_for_alignment =
      direct_map_metadata->direct_map_extent.padding_for_alignment;
  PA_DCHECK(padding_for_alignment ==
            static_cast<size_t>(page_metadata - first_page_metadata) *
                PartitionPageSize());
  PA_DCHECK(slot_start ==
            reservation_start + PartitionPageSize() + padding_for_alignment);
#endif  // BUILDFLAG(PA_DCHECK_IS_ON)
  return std::make_pair(slot_start, slot_span->bucket->slot_size);
}

// Gets the start address and size of the allocated slot. The input |address|
// can point anywhere in the slot, including the slot start as well as
// immediately past the slot.
//
// This isn't a general purpose function, it is used specifically for obtaining
// BackupRefPtr's ref-count. The caller is responsible for ensuring that the
// ref-count is in place for this allocation.
PA_ALWAYS_INLINE std::pair<uintptr_t, size_t>
PartitionAllocGetSlotStartAndSizeInBRPPool(uintptr_t address) {
  PA_DCHECK(IsManagedByNormalBucketsOrDirectMap(address));
  DCheckIfManagedByPartitionAllocBRPPool(address);

  auto directmap_slot_info =
      PartitionAllocGetDirectMapSlotStartAndSizeInBRPPool(address);
  if (PA_UNLIKELY(directmap_slot_info.first)) {
    return directmap_slot_info;
  }

  auto* slot_span = SlotSpanMetadata::FromAddr(address);
#if BUILDFLAG(PA_DCHECK_IS_ON)
  auto* root = PartitionRoot::FromSlotSpanMetadata(slot_span);
  // Double check that ref-count is indeed present.
  PA_DCHECK(root->brp_enabled());
#endif  // BUILDFLAG(PA_DCHECK_IS_ON)

  // Get the offset from the beginning of the slot span.
  uintptr_t slot_span_start = SlotSpanMetadata::ToSlotSpanStart(slot_span);
  size_t offset_in_slot_span = address - slot_span_start;

  auto* bucket = slot_span->bucket;
  return std::make_pair(
      slot_span_start +
          bucket->slot_size * bucket->GetSlotNumber(offset_in_slot_span),
      bucket->slot_size);
}

// Return values to indicate where a pointer is pointing relative to the bounds
// of an allocation.
enum class PtrPosWithinAlloc {
  // When BACKUP_REF_PTR_POISON_OOB_PTR is disabled, end-of-allocation pointers
  // are also considered in-bounds.
  kInBounds,
#if BUILDFLAG(BACKUP_REF_PTR_POISON_OOB_PTR)
  kAllocEnd,
#endif
  kFarOOB
};

// Checks whether `test_address` is in the same allocation slot as
// `orig_address`.
//
// This can be called after adding or subtracting from the `orig_address`
// to produce a different pointer which must still stay in the same allocation.
//
// The `type_size` is the size of the type that the raw_ptr is pointing to,
// which may be the type the allocation is holding or a compatible pointer type
// such as a base class or char*. It is used to detect pointers near the end of
// the allocation but not strictly beyond it.
//
// This isn't a general purpose function. The caller is responsible for ensuring
// that the ref-count is in place for this allocation.
PA_COMPONENT_EXPORT(PARTITION_ALLOC)
PtrPosWithinAlloc IsPtrWithinSameAlloc(uintptr_t orig_address,
                                       uintptr_t test_address,
                                       size_t type_size);

PA_ALWAYS_INLINE void PartitionAllocFreeForRefCounting(uintptr_t slot_start) {
  auto* slot_span = SlotSpanMetadata::FromSlotStart(slot_start);
  auto* root = PartitionRoot::FromSlotSpanMetadata(slot_span);
  // PartitionRefCount is required to be allocated inside a `PartitionRoot` that
  // supports reference counts.
  PA_DCHECK(root->brp_enabled());
  PA_DCHECK(!PartitionRoot::RefCountPointerFromSlotStartAndSize(
                 slot_start, slot_span->bucket->slot_size)
                 ->IsAlive());

  // Iterating over the entire slot can be really expensive.
#if BUILDFLAG(PA_EXPENSIVE_DCHECKS_ARE_ON)
  auto hook = PartitionAllocHooks::GetQuarantineOverrideHook();
  // If we have a hook the object segment is not necessarily filled
  // with |kQuarantinedByte|.
  if (PA_LIKELY(!hook)) {
    unsigned char* object =
        static_cast<unsigned char*>(root->SlotStartToObject(slot_start));
    for (size_t i = 0; i < root->GetSlotUsableSize(slot_span); ++i) {
      PA_DCHECK(object[i] == kQuarantinedByte);
    }
  }
  // TODO(crbug.com/1511221): Memset entire slot in the "same slot" mode.
  // Ref-count isn't used once the slot is freed.
  DebugMemset(SlotStartAddr2Ptr(slot_start), kFreedByte,
              slot_span->GetUtilizedSlotSize() - kInSlotRefCountBufferSize);
#endif  // BUILDFLAG(PA_EXPENSIVE_DCHECKS_ARE_ON)

  root->total_size_of_brp_quarantined_bytes.fetch_sub(
      slot_span->GetSlotSizeForBookkeeping(), std::memory_order_relaxed);
  root->total_count_of_brp_quarantined_slots.fetch_sub(
      1, std::memory_order_relaxed);

  root->RawFreeWithThreadCache(slot_start, slot_span);
}
#endif  // BUILDFLAG(ENABLE_BACKUP_REF_PTR_SUPPORT)

}  // namespace internal

template <AllocFlags flags>
PA_ALWAYS_INLINE uintptr_t
PartitionRoot::AllocFromBucket(Bucket* bucket,
                               size_t raw_size,
                               size_t slot_span_alignment,
                               size_t* usable_size,
                               size_t* slot_size,
                               bool* is_already_zeroed) {
  PA_DCHECK((slot_span_alignment >= internal::PartitionPageSize()) &&
            std::has_single_bit(slot_span_alignment));
  SlotSpanMetadata* slot_span = bucket->active_slot_spans_head;
  // There always must be a slot span on the active list (could be a sentinel).
  PA_DCHECK(slot_span);
  // Check that it isn't marked full, which could only be true if the span was
  // removed from the active list.
  PA_DCHECK(!slot_span->marked_full);

  uintptr_t slot_start =
      internal::SlotStartPtr2Addr(slot_span->get_freelist_head());
  // Use the fast path when a slot is readily available on the free list of the
  // first active slot span. However, fall back to the slow path if a
  // higher-order alignment is requested, because an inner slot of an existing
  // slot span is unlikely to satisfy it.
  if (PA_LIKELY(slot_span_alignment <= internal::PartitionPageSize() &&
                slot_start)) {
    *is_already_zeroed = false;
    // This is a fast path, avoid calling GetSlotUsableSize() in Release builds
    // as it is costlier. Copy its small bucket path instead.
    *usable_size = AdjustSizeForExtrasSubtract(bucket->slot_size);
    PA_DCHECK(*usable_size == GetSlotUsableSize(slot_span));

    // If these DCHECKs fire, you probably corrupted memory.
    // TODO(crbug.com/1257655): See if we can afford to make these CHECKs.
    DCheckIsValidSlotSpan(slot_span);

    // All large allocations must go through the slow path to correctly update
    // the size metadata.
    PA_DCHECK(!slot_span->CanStoreRawSize());
    PA_DCHECK(!slot_span->bucket->is_direct_mapped());
    void* entry = slot_span->PopForAlloc(bucket->slot_size);
    PA_DCHECK(internal::SlotStartPtr2Addr(entry) == slot_start);

    PA_DCHECK(slot_span->bucket == bucket);
  } else {
    slot_start =
        bucket->SlowPathAlloc(this, flags, raw_size, slot_span_alignment,
                              &slot_span, is_already_zeroed);
    if (PA_UNLIKELY(!slot_start)) {
      return 0;
    }
    PA_DCHECK(slot_span == SlotSpanMetadata::FromSlotStart(slot_start));
    // TODO(crbug.com/1257655): See if we can afford to make this a CHECK.
    DCheckIsValidSlotSpan(slot_span);
    // For direct mapped allocations, |bucket| is the sentinel.
    PA_DCHECK((slot_span->bucket == bucket) ||
              (slot_span->bucket->is_direct_mapped() &&
               (bucket == &sentinel_bucket)));

    *usable_size = GetSlotUsableSize(slot_span);
  }
  PA_DCHECK(slot_span->GetUtilizedSlotSize() <= slot_span->bucket->slot_size);
  IncreaseTotalSizeOfAllocatedBytes(
      slot_start, slot_span->GetSlotSizeForBookkeeping(), raw_size);

#if BUILDFLAG(USE_FREESLOT_BITMAP)
  if (!slot_span->bucket->is_direct_mapped()) {
    internal::FreeSlotBitmapMarkSlotAsUsed(slot_start);
  }
#endif

  *slot_size = slot_span->bucket->slot_size;
  return slot_start;
}

AllocationNotificationData PartitionRoot::CreateAllocationNotificationData(
    void* object,
    size_t size,
    const char* type_name) const {
  AllocationNotificationData notification_data(object, size, type_name);

  if (IsMemoryTaggingEnabled()) {
#if BUILDFLAG(HAS_MEMORY_TAGGING)
    notification_data.SetMteReportingMode(memory_tagging_reporting_mode());
#endif
  }

  return notification_data;
}

FreeNotificationData PartitionRoot::CreateDefaultFreeNotificationData(
    void* address) {
  return FreeNotificationData(address);
}

FreeNotificationData PartitionRoot::CreateFreeNotificationData(
    void* address) const {
  FreeNotificationData notification_data =
      CreateDefaultFreeNotificationData(address);

  if (IsMemoryTaggingEnabled()) {
#if BUILDFLAG(HAS_MEMORY_TAGGING)
    notification_data.SetMteReportingMode(memory_tagging_reporting_mode());
#endif
  }

  return notification_data;
}

// static
template <FreeFlags flags>
PA_ALWAYS_INLINE bool PartitionRoot::FreeProlog(void* object,
                                                const PartitionRoot* root) {
  static_assert(AreValidFlags(flags));
  if constexpr (ContainsFlags(flags, FreeFlags::kNoHooks)) {
    return false;
  }

#if defined(MEMORY_TOOL_REPLACES_ALLOCATOR)
  if constexpr (!ContainsFlags(flags, FreeFlags::kNoMemoryToolOverride)) {
    free(object);
    return true;
  }
#endif  // defined(MEMORY_TOOL_REPLACES_ALLOCATOR)
  if (PA_UNLIKELY(!object)) {
    return true;
  }

  if (PartitionAllocHooks::AreHooksEnabled()) {
    // A valid |root| might not be available if this function is called from
    // |FreeInUnknownRoot| and not deducible if object originates from
    // an override hook.
    // TODO(crbug.com/1137393): See if we can make the root available more
    // reliably or even make this function non-static.
    auto notification_data = root ? root->CreateFreeNotificationData(object)
                                  : CreateDefaultFreeNotificationData(object);
    PartitionAllocHooks::FreeObserverHookIfEnabled(notification_data);
    if (PartitionAllocHooks::FreeOverrideHookIfEnabled(object)) {
      return true;
    }
  }

  return false;
}

PA_ALWAYS_INLINE bool PartitionRoot::IsMemoryTaggingEnabled() const {
#if BUILDFLAG(HAS_MEMORY_TAGGING)
  return settings.memory_tagging_enabled_;
#else
  return false;
#endif  // BUILDFLAG(HAS_MEMORY_TAGGING)
}

PA_ALWAYS_INLINE TagViolationReportingMode
PartitionRoot::memory_tagging_reporting_mode() const {
#if BUILDFLAG(HAS_MEMORY_TAGGING)
  return settings.memory_tagging_reporting_mode_;
#else
  return TagViolationReportingMode::kUndefined;
#endif  // BUILDFLAG(HAS_MEMORY_TAGGING)
}

// static
template <FreeFlags flags>
PA_ALWAYS_INLINE void PartitionRoot::FreeInlineInUnknownRoot(void* object) {
  bool early_return = FreeProlog<flags>(object, nullptr);
  if (early_return) {
    return;
  }

  if (PA_UNLIKELY(!object)) {
    return;
  }

  // Fetch the root from the address, and not SlotSpanMetadata. This is
  // important, as obtaining it from SlotSpanMetadata is a slow operation
  // (looking into the metadata area, and following a pointer), which can induce
  // cache coherency traffic (since they're read on every free(), and written to
  // on any malloc()/free() that is not a hit in the thread cache). This way we
  // change the critical path from object -> slot_span -> root into two
  // *parallel* ones:
  // 1. object -> root
  // 2. object -> slot_span (inside FreeInline)
  uintptr_t object_addr = internal::ObjectPtr2Addr(object);
  auto* root = FromAddrInFirstSuperpage(object_addr);
  root->FreeInline<flags | FreeFlags::kNoHooks>(object);
}

template <FreeFlags flags>
PA_ALWAYS_INLINE void PartitionRoot::FreeInline(void* object) {
  // The correct PartitionRoot might not be deducible if the |object| originates
  // from an override hook.
  bool early_return = FreeProlog<flags>(object, this);
  if (early_return) {
    return;
  }

  if (PA_UNLIKELY(!object)) {
    return;
  }

  // Almost all calls to FreeNoNooks() will end up writing to |*object|, the
  // only cases where we don't would be delayed free() in PCScan, but |*object|
  // can be cold in cache.
  PA_PREFETCH(object);

  // On Android, malloc() interception is more fragile than on other
  // platforms, as we use wrapped symbols. However, the pools allow us to
  // quickly tell that a pointer was allocated with PartitionAlloc.
  //
  // This is a crash to detect imperfect symbol interception. However, we can
  // forward allocations we don't own to the system malloc() implementation in
  // these rare cases, assuming that some remain.
  //
  // On Android Chromecast devices, this is already checked in PartitionFree()
  // in the shim.
#if BUILDFLAG(USE_PARTITION_ALLOC_AS_MALLOC) && \
    (BUILDFLAG(IS_ANDROID) && !BUILDFLAG(PA_IS_CAST_ANDROID))
  uintptr_t object_addr = internal::ObjectPtr2Addr(object);
  PA_CHECK(IsManagedByPartitionAlloc(object_addr));
#endif

  SlotSpanMetadata* slot_span = SlotSpanMetadata::FromObject(object);
  PA_DCHECK(PartitionRoot::FromSlotSpanMetadata(slot_span) == this);

#if BUILDFLAG(HAS_MEMORY_TAGGING)
  if (PA_LIKELY(IsMemoryTaggingEnabled())) {
    const size_t slot_size = slot_span->bucket->slot_size;
    if (PA_LIKELY(slot_size <= internal::kMaxMemoryTaggingSize)) {
      // slot_span is untagged at this point, so we have to recover its tag
      // again to increment and provide use-after-free mitigations.
      void* retagged_slot_start = internal::TagMemoryRangeIncrement(
          ObjectToTaggedSlotStart(object), TagSizeForSlot(slot_size));
      // Incrementing the MTE-tag in the memory range invalidates the |object|'s
      // tag, so it must be retagged.
      object = TaggedSlotStartToObject(retagged_slot_start);
    }
  }
#else   // BUILDFLAG(HAS_MEMORY_TAGGING)
  // We are going to read from |*slot_span| in all branches, but haven't done it
  // yet.
  //
  // TODO(crbug.com/1207307): It would be much better to avoid touching
  // |*slot_span| at all on the fast path, or at least to separate its read-only
  // parts (i.e. bucket pointer) from the rest. Indeed, every thread cache miss
  // (or batch fill) will *write* to |slot_span->freelist_head|, leading to
  // cacheline ping-pong.
  //
  // Don't do it when memory tagging is enabled, as |*slot_span| has already
  // been touched above.
  PA_PREFETCH(slot_span);
#endif  // BUILDFLAG(HAS_MEMORY_TAGGING)

  uintptr_t slot_start = ObjectToSlotStart(object);
  PA_DCHECK(slot_span == SlotSpanMetadata::FromSlotStart(slot_start));

  if constexpr (ContainsFlags(flags, FreeFlags::kZap)) {
    if (settings.zapping_by_free_flags) {
      internal::SecureMemset(internal::SlotStartAddr2Ptr(slot_start),
                             internal::kFreedByte,
                             GetSlotUsableSize(slot_span));
    }
  }
  // TODO(https://crbug.com/1497380): Collecting objects for
  // `kSchedulerLoopQuarantineBranch` here means it "delays" other checks (BRP
  // refcount, cookie, etc.)
  // For better debuggability, we should do these checks before quarantining.
  if constexpr (ContainsFlags(flags, FreeFlags::kSchedulerLoopQuarantine)) {
    if (settings.scheduler_loop_quarantine) {
      GetSchedulerLoopQuarantineBranch().Quarantine(object, slot_span,
                                                    slot_start);
    }
    return;
  }

#if BUILDFLAG(USE_STARSCAN)
  // TODO(bikineev): Change the condition to PA_LIKELY once PCScan is enabled by
  // default.
  if (PA_UNLIKELY(ShouldQuarantine(object))) {
    // PCScan safepoint. Call before potentially scheduling scanning task.
    PCScan::JoinScanIfNeeded();
    if (PA_LIKELY(internal::IsManagedByNormalBuckets(slot_start))) {
      PCScan::MoveToQuarantine(object, GetSlotUsableSize(slot_span), slot_start,
                               slot_span->bucket->slot_size);
      return;
    }
  }
#endif  // BUILDFLAG(USE_STARSCAN)

  FreeNoHooksImmediate(object, slot_span, slot_start);
}

PA_ALWAYS_INLINE void PartitionRoot::FreeNoHooksImmediate(
    void* object,
    SlotSpanMetadata* slot_span,
    uintptr_t slot_start) {
  // The thread cache is added "in the middle" of the main allocator, that is:
  // - After all the cookie/ref-count management
  // - Before the "raw" allocator.
  //
  // On the deallocation side:
  // 1. Check cookie/ref-count, adjust the pointer
  // 2. Deallocation
  //   a. Return to the thread cache if possible. If it succeeds, return.
  //   b. Otherwise, call the "raw" allocator <-- Locking
  PA_DCHECK(object);
  PA_DCHECK(slot_span);
  DCheckIsValidSlotSpan(slot_span);
  PA_DCHECK(slot_start);

  // Layout inside the slot:
  //   |...object...|[empty]|[cookie]|[unused]|[refcnt]|
  //   <--------(a)--------->
  //                        <--(b)--->   +    <--(b)--->
  //   <-------------(c)------------->   +    <--(c)--->
  //     (a) usable_size
  //     (b) extras
  //     (c) utilized_slot_size
  //
  // Note: ref-count and cookie can be 0-sized.
  //
  // For more context, see the other "Layout inside the slot" comment inside
  // AllocInternalNoHooks().

  if (settings.use_cookie) {
    // Verify the cookie after the allocated region.
    // If this assert fires, you probably corrupted memory.
    internal::PartitionCookieCheckValue(static_cast<unsigned char*>(object) +
                                        GetSlotUsableSize(slot_span));
  }

#if BUILDFLAG(USE_STARSCAN)
  // TODO(bikineev): Change the condition to PA_LIKELY once PCScan is enabled by
  // default.
  if (PA_UNLIKELY(IsQuarantineEnabled())) {
    if (PA_LIKELY(internal::IsManagedByNormalBuckets(slot_start))) {
      // Mark the state in the state bitmap as freed.
      internal::StateBitmapFromAddr(slot_start)->Free(slot_start);
    }
  }
#endif  // BUILDFLAG(USE_STARSCAN)

#if BUILDFLAG(ENABLE_BACKUP_REF_PTR_SUPPORT)
  // TODO(keishi): Add PA_LIKELY when brp is fully enabled as |brp_enabled| will
  // be false only for the aligned partition.
  if (brp_enabled()) {
    auto* ref_count = RefCountPointerFromSlotStartAndSize(
        slot_start, slot_span->bucket->slot_size);
    // If there are no more references to the allocation, it can be freed
    // immediately. Otherwise, defer the operation and zap the memory to turn
    // potential use-after-free issues into unexploitable crashes.
    if (PA_UNLIKELY(!ref_count->IsAliveWithNoKnownRefs())) {
      auto usable_size = GetSlotUsableSize(slot_span);
      auto hook = PartitionAllocHooks::GetQuarantineOverrideHook();
      if (PA_UNLIKELY(hook)) {
        hook(object, usable_size);
      } else {
        internal::SecureMemset(object, internal::kQuarantinedByte, usable_size);
      }
    }

    if (PA_UNLIKELY(!(ref_count->ReleaseFromAllocator()))) {
      total_size_of_brp_quarantined_bytes.fetch_add(
          slot_span->GetSlotSizeForBookkeeping(), std::memory_order_relaxed);
      total_count_of_brp_quarantined_slots.fetch_add(1,
                                                     std::memory_order_relaxed);
      cumulative_size_of_brp_quarantined_bytes.fetch_add(
          slot_span->GetSlotSizeForBookkeeping(), std::memory_order_relaxed);
      cumulative_count_of_brp_quarantined_slots.fetch_add(
          1, std::memory_order_relaxed);
      return;
    }
  }
#endif  // BUILDFLAG(ENABLE_BACKUP_REF_PTR_SUPPORT)

  // memset() can be really expensive.
#if BUILDFLAG(PA_EXPENSIVE_DCHECKS_ARE_ON)
  // TODO(crbug.com/1511221): Memset entire slot in the "same slot" mode.
  // Ref-count isn't used once the slot is freed.
  internal::DebugMemset(
      internal::SlotStartAddr2Ptr(slot_start), internal::kFreedByte,
      slot_span->GetUtilizedSlotSize() - internal::kInSlotRefCountBufferSize);
#elif PA_CONFIG(ZERO_RANDOMLY_ON_FREE)
  // `memset` only once in a while: we're trading off safety for time
  // efficiency.
  if (PA_UNLIKELY(internal::RandomPeriod()) &&
      !IsDirectMappedBucket(slot_span->bucket)) {
    // TODO(crbug.com/1511221): Memset entire slot in the "same slot" mode.
    // Ref-count isn't used once the slot is freed.
    internal::SecureMemset(
        internal::SlotStartAddr2Ptr(slot_start), 0,
        slot_span->GetUtilizedSlotSize() - internal::kInSlotRefCountBufferSize);
  }
#endif  // PA_CONFIG(ZERO_RANDOMLY_ON_FREE)

  RawFreeWithThreadCache(slot_start, slot_span);
}

PA_ALWAYS_INLINE void PartitionRoot::FreeInSlotSpan(
    uintptr_t slot_start,
    SlotSpanMetadata* slot_span) {
  DecreaseTotalSizeOfAllocatedBytes(slot_start,
                                    slot_span->GetSlotSizeForBookkeeping());
#if BUILDFLAG(USE_FREESLOT_BITMAP)
  if (!slot_span->bucket->is_direct_mapped()) {
    internal::FreeSlotBitmapMarkSlotAsFree(slot_start);
  }
#endif

  return slot_span->Free(slot_start, this);
}

PA_ALWAYS_INLINE void PartitionRoot::RawFree(uintptr_t slot_start) {
  SlotSpanMetadata* slot_span = SlotSpanMetadata::FromSlotStart(slot_start);
  RawFree(slot_start, slot_span);
}

#if PA_CONFIG(IS_NONCLANG_MSVC)
// MSVC only supports inline assembly on x86. This preprocessor directive
// is intended to be a replacement for the same.
//
// TODO(crbug.com/1351310): Make sure inlining doesn't degrade this into
// a no-op or similar. The documentation doesn't say.
#pragma optimize("", off)
#endif
PA_ALWAYS_INLINE void PartitionRoot::RawFree(uintptr_t slot_start,
                                             SlotSpanMetadata* slot_span) {
  // At this point we are about to acquire the lock, so we try to minimize the
  // risk of blocking inside the locked section.
  //
  // For allocations that are not direct-mapped, there will always be a store at
  // the beginning of |*slot_start|, to link the freelist. This is why there is
  // a prefetch of it at the beginning of the free() path.
  //
  // However, the memory which is being freed can be very cold (for instance
  // during browser shutdown, when various caches are finally completely freed),
  // and so moved to either compressed memory or swap. This means that touching
  // it here can cause a major page fault. This is in turn will cause
  // descheduling of the thread *while locked*. Since we don't have priority
  // inheritance locks on most platforms, avoiding long locked periods relies on
  // the OS having proper priority boosting. There is evidence
  // (crbug.com/1228523) that this is not always the case on Windows, and a very
  // low priority background thread can block the main one for a long time,
  // leading to hangs.
  //
  // To mitigate that, make sure that we fault *before* locking. Note that this
  // is useless for direct-mapped allocations (which are very rare anyway), and
  // that this path is *not* taken for thread cache bucket purge (since it calls
  // RawFreeLocked()). This is intentional, as the thread cache is purged often,
  // and the memory has a consequence the memory has already been touched
  // recently (to link the thread cache freelist).
  *static_cast<volatile uintptr_t*>(internal::SlotStartAddr2Ptr(slot_start)) =
      0;
  // Note: even though we write to slot_start + sizeof(void*) as well, due to
  // alignment constraints, the two locations are always going to be in the same
  // OS page. No need to write to the second one as well.
  //
  // Do not move the store above inside the locked section.
#if !(PA_CONFIG(IS_NONCLANG_MSVC))
  __asm__ __volatile__("" : : "r"(slot_start) : "memory");
#endif

  ::partition_alloc::internal::ScopedGuard guard{
      internal::PartitionRootLock(this)};
  FreeInSlotSpan(slot_start, slot_span);
}
#if PA_CONFIG(IS_NONCLANG_MSVC)
#pragma optimize("", on)
#endif

PA_ALWAYS_INLINE void PartitionRoot::RawFreeBatch(FreeListEntry* head,
                                                  FreeListEntry* tail,
                                                  size_t size,
                                                  SlotSpanMetadata* slot_span) {
  PA_DCHECK(head);
  PA_DCHECK(tail);
  PA_DCHECK(size > 0);
  PA_DCHECK(slot_span);
  DCheckIsValidSlotSpan(slot_span);
  // The passed freelist is likely to be just built up, which means that the
  // corresponding pages were faulted in (without acquiring the lock). So there
  // is no need to touch pages manually here before the lock.
  ::partition_alloc::internal::ScopedGuard guard{
      internal::PartitionRootLock(this)};
  // TODO(thiabaud): Fix the accounting here. The size is correct, but the
  // pointer is not. This only affects local tools that record each allocation,
  // not our metrics.
  DecreaseTotalSizeOfAllocatedBytes(
      0u, slot_span->GetSlotSizeForBookkeeping() * size);
  slot_span->AppendFreeList(head, tail, size, this);
}

PA_ALWAYS_INLINE void PartitionRoot::RawFreeWithThreadCache(
    uintptr_t slot_start,
    SlotSpanMetadata* slot_span) {
  // PA_LIKELY: performance-sensitive partitions have a thread cache,
  // direct-mapped allocations are uncommon.
  ThreadCache* thread_cache = GetThreadCache();
  if (PA_LIKELY(ThreadCache::IsValid(thread_cache) &&
                !IsDirectMappedBucket(slot_span->bucket))) {
    size_t bucket_index =
        static_cast<size_t>(slot_span->bucket - this->buckets);
    size_t slot_size;
    if (PA_LIKELY(thread_cache->MaybePutInCache(slot_start, bucket_index,
                                                &slot_size))) {
      // This is a fast path, avoid calling GetSlotUsableSize() in Release
      // builds as it is costlier. Copy its small bucket path instead.
      PA_DCHECK(!slot_span->CanStoreRawSize());
      size_t usable_size = AdjustSizeForExtrasSubtract(slot_size);
      PA_DCHECK(usable_size == GetSlotUsableSize(slot_span));
      thread_cache->RecordDeallocation(usable_size);
      return;
    }
  }

  if (PA_LIKELY(ThreadCache::IsValid(thread_cache))) {
    // Accounting must be done outside `RawFree()`, as it's also called from the
    // thread cache. We would double-count otherwise.
    //
    // GetSlotUsableSize() will always give the correct result, and we are in
    // a slow path here (since the thread cache case returned earlier).
    size_t usable_size = GetSlotUsableSize(slot_span);
    thread_cache->RecordDeallocation(usable_size);
  }
  RawFree(slot_start, slot_span);
}

PA_ALWAYS_INLINE void PartitionRoot::RawFreeLocked(uintptr_t slot_start) {
  SlotSpanMetadata* slot_span = SlotSpanMetadata::FromSlotStart(slot_start);
  // Direct-mapped deallocation releases then re-acquires the lock. The caller
  // may not expect that, but we never call this function on direct-mapped
  // allocations.
  PA_DCHECK(!IsDirectMappedBucket(slot_span->bucket));
  FreeInSlotSpan(slot_start, slot_span);
}

PA_ALWAYS_INLINE PartitionRoot* PartitionRoot::FromSlotSpanMetadata(
    SlotSpanMetadata* slot_span) {
  auto* extent_entry = reinterpret_cast<SuperPageExtentEntry*>(
      reinterpret_cast<uintptr_t>(slot_span) & internal::SystemPageBaseMask());
  return extent_entry->root;
}

PA_ALWAYS_INLINE PartitionRoot* PartitionRoot::FromFirstSuperPage(
    uintptr_t super_page) {
  PA_DCHECK(internal::IsReservationStart(super_page));
  auto* extent_entry = internal::PartitionSuperPageToExtent(super_page);
  PartitionRoot* root = extent_entry->root;
  PA_DCHECK(root->inverted_self == ~reinterpret_cast<uintptr_t>(root));
  return root;
}

PA_ALWAYS_INLINE PartitionRoot* PartitionRoot::FromAddrInFirstSuperpage(
    uintptr_t address) {
  uintptr_t super_page = address & internal::kSuperPageBaseMask;
  PA_DCHECK(internal::IsReservationStart(super_page));
  return FromFirstSuperPage(super_page);
}

PA_ALWAYS_INLINE void PartitionRoot::IncreaseTotalSizeOfAllocatedBytes(
    uintptr_t addr,
    size_t len,
    size_t raw_size) {
  total_size_of_allocated_bytes += len;
  max_size_of_allocated_bytes =
      std::max(max_size_of_allocated_bytes, total_size_of_allocated_bytes);
#if BUILDFLAG(RECORD_ALLOC_INFO)
  partition_alloc::internal::RecordAllocOrFree(addr | 0x01, raw_size);
#endif  // BUILDFLAG(RECORD_ALLOC_INFO)
}

PA_ALWAYS_INLINE void PartitionRoot::DecreaseTotalSizeOfAllocatedBytes(
    uintptr_t addr,
    size_t len) {
  // An underflow here means we've miscounted |total_size_of_allocated_bytes|
  // somewhere.
  PA_DCHECK(total_size_of_allocated_bytes >= len);
  total_size_of_allocated_bytes -= len;
#if BUILDFLAG(RECORD_ALLOC_INFO)
  partition_alloc::internal::RecordAllocOrFree(addr | 0x00, len);
#endif  // BUILDFLAG(RECORD_ALLOC_INFO)
}

PA_ALWAYS_INLINE void PartitionRoot::IncreaseCommittedPages(size_t len) {
  const auto old_total =
      total_size_of_committed_pages.fetch_add(len, std::memory_order_relaxed);

  const auto new_total = old_total + len;

  // This function is called quite frequently; to avoid performance problems, we
  // don't want to hold a lock here, so we use compare and exchange instead.
  size_t expected = max_size_of_committed_pages.load(std::memory_order_relaxed);
  size_t desired;
  do {
    desired = std::max(expected, new_total);
  } while (!max_size_of_committed_pages.compare_exchange_weak(
      expected, desired, std::memory_order_relaxed, std::memory_order_relaxed));
}

PA_ALWAYS_INLINE void PartitionRoot::DecreaseCommittedPages(size_t len) {
  total_size_of_committed_pages.fetch_sub(len, std::memory_order_relaxed);
}

PA_ALWAYS_INLINE void PartitionRoot::DecommitSystemPagesForData(
    uintptr_t address,
    size_t length,
    PageAccessibilityDisposition accessibility_disposition) {
  internal::ScopedSyscallTimer timer{this};
  DecommitSystemPages(address, length, accessibility_disposition);
  DecreaseCommittedPages(length);
}

// Not unified with TryRecommitSystemPagesForData() to preserve error codes.
PA_ALWAYS_INLINE void PartitionRoot::RecommitSystemPagesForData(
    uintptr_t address,
    size_t length,
    PageAccessibilityDisposition accessibility_disposition,
    bool request_tagging) {
  internal::ScopedSyscallTimer timer{this};

  auto page_accessibility = GetPageAccessibility(request_tagging);
  bool ok = TryRecommitSystemPages(address, length, page_accessibility,
                                   accessibility_disposition);
  if (PA_UNLIKELY(!ok)) {
    // Decommit some memory and retry. The alternative is crashing.
    DecommitEmptySlotSpans();
    RecommitSystemPages(address, length, page_accessibility,
                        accessibility_disposition);
  }

  IncreaseCommittedPages(length);
}

template <bool already_locked>
PA_ALWAYS_INLINE bool PartitionRoot::TryRecommitSystemPagesForDataInternal(
    uintptr_t address,
    size_t length,
    PageAccessibilityDisposition accessibility_disposition,
    bool request_tagging) {
  internal::ScopedSyscallTimer timer{this};

  auto page_accessibility = GetPageAccessibility(request_tagging);
  bool ok = TryRecommitSystemPages(address, length, page_accessibility,
                                   accessibility_disposition);
  if (PA_UNLIKELY(!ok)) {
    {
      // Decommit some memory and retry. The alternative is crashing.
      if constexpr (!already_locked) {
        ::partition_alloc::internal::ScopedGuard guard(
            internal::PartitionRootLock(this));
        DecommitEmptySlotSpans();
      } else {
        internal::PartitionRootLock(this).AssertAcquired();
        DecommitEmptySlotSpans();
      }
    }
    ok = TryRecommitSystemPages(address, length, page_accessibility,
                                accessibility_disposition);
  }

  if (ok) {
    IncreaseCommittedPages(length);
  }

  return ok;
}

PA_ALWAYS_INLINE bool
PartitionRoot::TryRecommitSystemPagesForDataWithAcquiringLock(
    uintptr_t address,
    size_t length,
    PageAccessibilityDisposition accessibility_disposition,
    bool request_tagging) {
  return TryRecommitSystemPagesForDataInternal<false>(
      address, length, accessibility_disposition, request_tagging);
}

PA_ALWAYS_INLINE
bool PartitionRoot::TryRecommitSystemPagesForDataLocked(
    uintptr_t address,
    size_t length,
    PageAccessibilityDisposition accessibility_disposition,
    bool request_tagging) {
  return TryRecommitSystemPagesForDataInternal<true>(
      address, length, accessibility_disposition, request_tagging);
}

// static
//
// Returns the size available to the app. It can be equal or higher than the
// requested size. If higher, the overage won't exceed what's actually usable
// by the app without a risk of running out of an allocated region or into
// PartitionAlloc's internal data. Used as malloc_usable_size and malloc_size.
//
// |ptr| should preferably point to the beginning of an object returned from
// malloc() et al., but it doesn't have to. crbug.com/1292646 shows an example
// where this isn't the case. Note, an inner object pointer won't work for
// direct map, unless it is within the first partition page.
PA_ALWAYS_INLINE size_t PartitionRoot::GetUsableSize(void* ptr) {
  // malloc_usable_size() is expected to handle NULL gracefully and return 0.
  if (!ptr) {
    return 0;
  }
  auto* slot_span = SlotSpanMetadata::FromObjectInnerPtr(ptr);
  auto* root = FromSlotSpanMetadata(slot_span);
  return root->GetSlotUsableSize(slot_span);
}

PA_ALWAYS_INLINE size_t
PartitionRoot::GetUsableSizeWithMac11MallocSizeHack(void* ptr) {
  // malloc_usable_size() is expected to handle NULL gracefully and return 0.
  if (!ptr) {
    return 0;
  }
  auto* slot_span = SlotSpanMetadata::FromObjectInnerPtr(ptr);
  auto* root = FromSlotSpanMetadata(slot_span);
  size_t usable_size = root->GetSlotUsableSize(slot_span);
#if PA_CONFIG(MAYBE_ENABLE_MAC11_MALLOC_SIZE_HACK)
  // Check |mac11_malloc_size_hack_enabled_| flag first as this doesn't
  // concern OS versions other than macOS 11.
  if (PA_UNLIKELY(root->settings.mac11_malloc_size_hack_enabled_ &&
                  usable_size ==
                      root->settings.mac11_malloc_size_hack_usable_size_)) {
    auto [slot_start, slot_size] =
        internal::PartitionAllocGetSlotStartAndSizeInBRPPool(UntagPtr(ptr));
    auto* ref_count =
        RefCountPointerFromSlotStartAndSize(slot_start, slot_size);
    if (ref_count->NeedsMac11MallocSizeHack()) {
      return internal::kMac11MallocSizeHackRequestedSize;
    }
  }
#endif  // PA_CONFIG(MAYBE_ENABLE_MAC11_MALLOC_SIZE_HACK)

  return usable_size;
}

// Returns the page configuration to use when mapping slot spans for a given
// partition root. ReadWriteTagged is used on MTE-enabled systems for
// PartitionRoots supporting it.
PA_ALWAYS_INLINE PageAccessibilityConfiguration
PartitionRoot::GetPageAccessibility(bool request_tagging) const {
  PageAccessibilityConfiguration::Permissions permissions =
      PageAccessibilityConfiguration::kReadWrite;
#if BUILDFLAG(HAS_MEMORY_TAGGING)
  if (IsMemoryTaggingEnabled() && request_tagging) {
    permissions = PageAccessibilityConfiguration::kReadWriteTagged;
  }
#endif
#if BUILDFLAG(ENABLE_THREAD_ISOLATION)
  return PageAccessibilityConfiguration(permissions, settings.thread_isolation);
#else
  return PageAccessibilityConfiguration(permissions);
#endif
}

PA_ALWAYS_INLINE PageAccessibilityConfiguration
PartitionRoot::PageAccessibilityWithThreadIsolationIfEnabled(
    PageAccessibilityConfiguration::Permissions permissions) const {
#if BUILDFLAG(ENABLE_THREAD_ISOLATION)
  return PageAccessibilityConfiguration(permissions, settings.thread_isolation);
#endif
  return PageAccessibilityConfiguration(permissions);
}

// Return the capacity of the underlying slot (adjusted for extras). This
// doesn't mean this capacity is readily available. It merely means that if
// a new allocation (or realloc) happened with that returned value, it'd use
// the same amount of underlying memory.
PA_ALWAYS_INLINE size_t
PartitionRoot::AllocationCapacityFromSlotStart(uintptr_t slot_start) const {
  auto* slot_span = SlotSpanMetadata::FromSlotStart(slot_start);
  return AdjustSizeForExtrasSubtract(slot_span->bucket->slot_size);
}

#if BUILDFLAG(ENABLE_BACKUP_REF_PTR_SUPPORT)
PA_ALWAYS_INLINE internal::PartitionRefCount*
PartitionRoot::RefCountPointerFromSlotStartAndSize(uintptr_t slot_start,
                                                   size_t slot_size) {
  return internal::PartitionRefCountPointer(slot_start, slot_size,
                                            ref_count_in_same_slot_);
}

PA_ALWAYS_INLINE internal::PartitionRefCount*
PartitionRoot::RefCountPointerFromObjectForTesting(void* object) const {
  uintptr_t slot_start = ObjectToSlotStart(object);
  auto* slot_span = SlotSpanMetadata::FromSlotStart(slot_start);
  return RefCountPointerFromSlotStartAndSize(slot_start,
                                             slot_span->bucket->slot_size);
}
#endif  // BUILDFLAG(ENABLE_BACKUP_REF_PTR_SUPPORT)

// static
PA_ALWAYS_INLINE uint16_t
PartitionRoot::SizeToBucketIndex(size_t size,
                                 BucketDistribution bucket_distribution) {
  switch (bucket_distribution) {
    case BucketDistribution::kNeutral:
      return internal::BucketIndexLookup::GetIndexForNeutralBuckets(size);
    case BucketDistribution::kDenser:
      return internal::BucketIndexLookup::GetIndexForDenserBuckets(size);
  }
}

template <AllocFlags flags>
PA_ALWAYS_INLINE void* PartitionRoot::AllocInternal(size_t requested_size,
                                                    size_t slot_span_alignment,
                                                    const char* type_name) {
  static_assert(AreValidFlags(flags));
  PA_DCHECK((slot_span_alignment >= internal::PartitionPageSize()) &&
            std::has_single_bit(slot_span_alignment));
  static_assert(!ContainsFlags(
      flags, AllocFlags::kMemoryShouldBeTaggedForMte));  // Internal only.

  constexpr bool no_hooks = ContainsFlags(flags, AllocFlags::kNoHooks);
  bool hooks_enabled;

  if constexpr (!no_hooks) {
    PA_DCHECK(initialized);

#if defined(MEMORY_TOOL_REPLACES_ALLOCATOR)
    if constexpr (!ContainsFlags(flags, AllocFlags::kNoMemoryToolOverride)) {
      if (!PartitionRoot::AllocWithMemoryToolProlog<flags>(requested_size)) {
        // Early return if AllocWithMemoryToolProlog returns false
        return nullptr;
      }
      constexpr bool zero_fill = ContainsFlags(flags, AllocFlags::kZeroFill);
      void* result =
          zero_fill ? calloc(1, requested_size) : malloc(requested_size);
      if constexpr (!ContainsFlags(flags, AllocFlags::kReturnNull)) {
        PA_CHECK(result);
      }
      return result;
    }
#endif  // defined(MEMORY_TOOL_REPLACES_ALLOCATOR)
    void* object = nullptr;
    hooks_enabled = PartitionAllocHooks::AreHooksEnabled();
    if (hooks_enabled) {
      auto additional_flags = AllocFlags::kNone;
#if BUILDFLAG(HAS_MEMORY_TAGGING)
      if (IsMemoryTaggingEnabled()) {
        additional_flags |= AllocFlags::kMemoryShouldBeTaggedForMte;
      }
#endif
      // The override hooks will return false if it can't handle the request,
      // i.e. due to unsupported flags. In this case, we forward the allocation
      // request to the default mechanisms.
      // TODO(crbug.com/1137393): See if we can make the forwarding more verbose
      // to ensure that this situation doesn't go unnoticed.
      if (PartitionAllocHooks::AllocationOverrideHookIfEnabled(
              &object, flags | additional_flags, requested_size, type_name)) {
        PartitionAllocHooks::AllocationObserverHookIfEnabled(
            CreateAllocationNotificationData(object, requested_size,
                                             type_name));
        return object;
      }
    }
  }

  void* const object =
      AllocInternalNoHooks<flags>(requested_size, slot_span_alignment);

  if constexpr (!no_hooks) {
    if (PA_UNLIKELY(hooks_enabled)) {
      PartitionAllocHooks::AllocationObserverHookIfEnabled(
          CreateAllocationNotificationData(object, requested_size, type_name));
    }
  }

  return object;
}

template <AllocFlags flags>
PA_ALWAYS_INLINE void* PartitionRoot::AllocInternalNoHooks(
    size_t requested_size,
    size_t slot_span_alignment) {
  static_assert(AreValidFlags(flags));

  // The thread cache is added "in the middle" of the main allocator, that is:
  // - After all the cookie/ref-count management
  // - Before the "raw" allocator.
  //
  // That is, the general allocation flow is:
  // 1. Adjustment of requested size to make room for extras
  // 2. Allocation:
  //   a. Call to the thread cache, if it succeeds, go to step 3.
  //   b. Otherwise, call the "raw" allocator <-- Locking
  // 3. Handle cookie/ref-count, zero allocation if required

  size_t raw_size = AdjustSizeForExtrasAdd(requested_size);
  PA_CHECK(raw_size >= requested_size);  // check for overflows

  // We should only call |SizeToBucketIndex| at most once when allocating.
  // Otherwise, we risk having |bucket_distribution| changed
  // underneath us (between calls to |SizeToBucketIndex| during the same call),
  // which would result in an inconsistent state.
  uint16_t bucket_index =
      SizeToBucketIndex(raw_size, this->GetBucketDistribution());
  size_t usable_size;
  bool is_already_zeroed = false;
  uintptr_t slot_start = 0;
  size_t slot_size = 0;

#if BUILDFLAG(USE_STARSCAN)
  const bool is_quarantine_enabled = IsQuarantineEnabled();
  // PCScan safepoint. Call before trying to allocate from cache.
  // TODO(bikineev): Change the condition to PA_LIKELY once PCScan is enabled by
  // default.
  if (PA_UNLIKELY(is_quarantine_enabled)) {
    PCScan::JoinScanIfNeeded();
  }
#endif  // BUILDFLAG(USE_STARSCAN)

  auto* thread_cache = GetOrCreateThreadCache();

  // Don't use thread cache if higher order alignment is requested, because the
  // thread cache will not be able to satisfy it.
  //
  // PA_LIKELY: performance-sensitive partitions use the thread cache.
  if (PA_LIKELY(ThreadCache::IsValid(thread_cache) &&
                slot_span_alignment <= internal::PartitionPageSize())) {
    // Note: getting slot_size from the thread cache rather than by
    // `buckets[bucket_index].slot_size` to avoid touching `buckets` on the fast
    // path.
    slot_start = thread_cache->GetFromCache(bucket_index, &slot_size);

    // PA_LIKELY: median hit rate in the thread cache is 95%, from metrics.
    if (PA_LIKELY(slot_start)) {
      // This follows the logic of SlotSpanMetadata::GetUsableSize for small
      // buckets, which is too expensive to call here.
      // Keep it in sync!
      usable_size = AdjustSizeForExtrasSubtract(slot_size);

#if BUILDFLAG(PA_DCHECK_IS_ON)
      // Make sure that the allocated pointer comes from the same place it would
      // for a non-thread cache allocation.
      SlotSpanMetadata* slot_span = SlotSpanMetadata::FromSlotStart(slot_start);
      DCheckIsValidSlotSpan(slot_span);
      PA_DCHECK(slot_span->bucket == &bucket_at(bucket_index));
      PA_DCHECK(slot_span->bucket->slot_size == slot_size);
      PA_DCHECK(usable_size == GetSlotUsableSize(slot_span));
      // All large allocations must go through the RawAlloc path to correctly
      // set |usable_size|.
      PA_DCHECK(!slot_span->CanStoreRawSize());
      PA_DCHECK(!slot_span->bucket->is_direct_mapped());
#endif
    } else {
      slot_start =
          RawAlloc<flags>(buckets + bucket_index, raw_size, slot_span_alignment,
                          &usable_size, &slot_size, &is_already_zeroed);
    }
  } else {
    slot_start =
        RawAlloc<flags>(buckets + bucket_index, raw_size, slot_span_alignment,
                        &usable_size, &slot_size, &is_already_zeroed);
  }

  if (PA_UNLIKELY(!slot_start)) {
    return nullptr;
  }

  if (PA_LIKELY(ThreadCache::IsValid(thread_cache))) {
    thread_cache->RecordAllocation(usable_size);
  }

  // Layout inside the slot:
  //   |...object...|[empty]|[cookie]|[unused]|[refcnt]|
  //   <----(a)----->
  //   <--------(b)--------->
  //                        <--(c)--->   +    <--(c)--->
  //   <----(d)----->   +   <--(d)--->   +    <--(d)--->
  //   <-------------(e)------------->   +    <--(e)--->
  //   <----------------------(f)---------------------->
  //     (a) requested_size
  //     (b) usable_size
  //     (c) extras
  //     (d) raw_size
  //     (e) utilized_slot_size
  //     (f) slot_size
  //
  // Notes:
  // - Cookie exists only in the BUILDFLAG(PA_DCHECK_IS_ON) case.
  // - Think of raw_size as the minimum size required internally to satisfy
  //   the allocation request (i.e. requested_size + extras)
  // - At most one "empty" or "unused" space can occur at a time. They occur
  //   when slot_size is larger than raw_size. "unused" applies only to large
  //   allocations (direct-mapped and single-slot slot spans) and "empty" only
  //   to small allocations.
  //   Why either-or, one might ask? We make an effort to put the trailing
  //   cookie as close to data as possible to catch overflows (often
  //   off-by-one), but that's possible only if we have enough space in metadata
  //   to save raw_size, i.e. only for large allocations. For small allocations,
  //   we have no other choice than putting the cookie at the very end of the
  //   slot, thus creating the "empty" space.
  // - Unlike "unused", "empty" counts towards usable_size, because the app can
  //   query for it and use this space without a need for reallocation.
  // - Ref-count may or may not exist in the slot, depending on
  //   ENABLE_BACKUP_REF_PTR_SUPPORT and brp_enabled().
  // - If slot_start is not SystemPageSize()-aligned (possible only for small
  //   allocations), ref-count is stored either at the end of the current slot
  //   or the previous slot, depending on the
  //   PartitionRoot::ref_count_in_same_slot_ setting. Otherwise it is stored in
  //   the ref-count table placed after the super page metadata. For simplicity,
  //   the space for ref-count is still reserved at the end of the slot, even
  //   though redundant.

  void* object = SlotStartToObject(slot_start);

  // Add the cookie after the allocation.
  if (settings.use_cookie) {
    internal::PartitionCookieWriteValue(static_cast<unsigned char*>(object) +
                                        usable_size);
  }

  // Fill the region kUninitializedByte (on debug builds, if not requested to 0)
  // or 0 (if requested and not 0 already).
  constexpr bool zero_fill = ContainsFlags(flags, AllocFlags::kZeroFill);
  // PA_LIKELY: operator new() calls malloc(), not calloc().
  if constexpr (!zero_fill) {
    // memset() can be really expensive.
#if BUILDFLAG(PA_EXPENSIVE_DCHECKS_ARE_ON)
    internal::DebugMemset(object, internal::kUninitializedByte, usable_size);
#endif
  } else if (!is_already_zeroed) {
    memset(object, 0, usable_size);
  }

#if BUILDFLAG(ENABLE_BACKUP_REF_PTR_SUPPORT)
  // TODO(keishi): Add PA_LIKELY when brp is fully enabled as |brp_enabled| will
  // be false only for the aligned partition.
  if (brp_enabled()) {
    bool needs_mac11_malloc_size_hack = false;
#if PA_CONFIG(MAYBE_ENABLE_MAC11_MALLOC_SIZE_HACK)
    // Only apply hack to size 32 allocations on macOS 11. There is a buggy
    // assertion that malloc_size() equals sizeof(class_rw_t) which is 32.
    if (PA_UNLIKELY(settings.mac11_malloc_size_hack_enabled_ &&
                    requested_size ==
                        internal::kMac11MallocSizeHackRequestedSize)) {
      needs_mac11_malloc_size_hack = true;
    }
#endif  // PA_CONFIG(MAYBE_ENABLE_MAC11_MALLOC_SIZE_HACK)
    auto* ref_count =
        new (RefCountPointerFromSlotStartAndSize(slot_start, slot_size))
            internal::PartitionRefCount(needs_mac11_malloc_size_hack);
#if PA_CONFIG(REF_COUNT_STORE_REQUESTED_SIZE)
    ref_count->SetRequestedSize(requested_size);
#else
    (void)ref_count;
#endif
  }
#endif  // BUILDFLAG(ENABLE_BACKUP_REF_PTR_SUPPORT)

#if BUILDFLAG(USE_STARSCAN)
  // TODO(bikineev): Change the condition to PA_LIKELY once PCScan is enabled by
  // default.
  if (PA_UNLIKELY(is_quarantine_enabled)) {
    if (PA_LIKELY(internal::IsManagedByNormalBuckets(slot_start))) {
      // Mark the corresponding bits in the state bitmap as allocated.
      internal::StateBitmapFromAddr(slot_start)->Allocate(slot_start);
    }
  }
#endif  // BUILDFLAG(USE_STARSCAN)

  return object;
}

template <AllocFlags flags>
PA_ALWAYS_INLINE uintptr_t PartitionRoot::RawAlloc(Bucket* bucket,
                                                   size_t raw_size,
                                                   size_t slot_span_alignment,
                                                   size_t* usable_size,
                                                   size_t* slot_size,
                                                   bool* is_already_zeroed) {
  ::partition_alloc::internal::ScopedGuard guard{
      internal::PartitionRootLock(this)};
  return AllocFromBucket<flags>(bucket, raw_size, slot_span_alignment,
                                usable_size, slot_size, is_already_zeroed);
}

template <AllocFlags flags>
PA_ALWAYS_INLINE void* PartitionRoot::AlignedAllocInline(
    size_t alignment,
    size_t requested_size) {
  // Aligned allocation support relies on the natural alignment guarantees of
  // PartitionAlloc. Specifically, it relies on the fact that slots within a
  // slot span are aligned to slot size, from the beginning of the span.
  //
  // For alignments <=PartitionPageSize(), the code below adjusts the request
  // size to be a power of two, no less than alignment. Since slot spans are
  // aligned to PartitionPageSize(), which is also a power of two, this will
  // automatically guarantee alignment on the adjusted size boundary, thanks to
  // the natural alignment described above.
  //
  // For alignments >PartitionPageSize(), we need to pass the request down the
  // stack to only give us a slot span aligned to this more restrictive
  // boundary. In the current implementation, this code path will always
  // allocate a new slot span and hand us the first slot, so no need to adjust
  // the request size. As a consequence, allocating many small objects with
  // such a high alignment can cause a non-negligable fragmentation,
  // particularly if these allocations are back to back.
  // TODO(bartekn): We should check that this is not causing issues in practice.
  //
  // This relies on the fact that there are no extras before the allocation, as
  // they'd shift the returned allocation from the beginning of the slot, thus
  // messing up alignment. Extras after the allocation are acceptable, but they
  // have to be taken into account in the request size calculation to avoid
  // crbug.com/1185484.

  // This is mandated by |posix_memalign()|, so should never fire.
  PA_CHECK(std::has_single_bit(alignment));
  // Catch unsupported alignment requests early.
  PA_CHECK(alignment <= internal::kMaxSupportedAlignment);
  size_t raw_size = AdjustSizeForExtrasAdd(requested_size);

  size_t adjusted_size = requested_size;
  if (alignment <= internal::PartitionPageSize()) {
    // Handle cases such as size = 16, alignment = 64.
    // Wastes memory when a large alignment is requested with a small size, but
    // this is hard to avoid, and should not be too common.
    if (PA_UNLIKELY(raw_size < alignment)) {
      raw_size = alignment;
    } else {
      // PartitionAlloc only guarantees alignment for power-of-two sized
      // allocations. To make sure this applies here, round up the allocation
      // size.
      raw_size = static_cast<size_t>(1)
                 << (int{sizeof(size_t) * 8} - std::countl_zero(raw_size - 1));
    }
    PA_DCHECK(std::has_single_bit(raw_size));
    // Adjust back, because AllocInternalNoHooks/Alloc will adjust it again.
    adjusted_size = AdjustSizeForExtrasSubtract(raw_size);

    // Overflow check. adjusted_size must be larger or equal to requested_size.
    if (PA_UNLIKELY(adjusted_size < requested_size)) {
      if constexpr (ContainsFlags(flags, AllocFlags::kReturnNull)) {
        return nullptr;
      }
      // OutOfMemoryDeathTest.AlignedAlloc requires
      // base::TerminateBecauseOutOfMemory (invoked by
      // PartitionExcessiveAllocationSize).
      internal::PartitionExcessiveAllocationSize(requested_size);
      // internal::PartitionExcessiveAllocationSize(size) causes OOM_CRASH.
      PA_NOTREACHED();
    }
  }

  // Slot spans are naturally aligned on partition page size, but make sure you
  // don't pass anything less, because it'll mess up callee's calculations.
  size_t slot_span_alignment =
      std::max(alignment, internal::PartitionPageSize());
  void* object =
      AllocInternal<flags>(adjusted_size, slot_span_alignment, nullptr);

  // |alignment| is a power of two, but the compiler doesn't necessarily know
  // that. A regular % operation is very slow, make sure to use the equivalent,
  // faster form.
  // No need to MTE-untag, as it doesn't change alignment.
  PA_CHECK(!(reinterpret_cast<uintptr_t>(object) & (alignment - 1)));

  return object;
}

template <AllocFlags alloc_flags, FreeFlags free_flags>
void* PartitionRoot::ReallocInline(void* ptr,
                                   size_t new_size,
                                   const char* type_name) {
#if defined(MEMORY_TOOL_REPLACES_ALLOCATOR)
  if (!PartitionRoot::AllocWithMemoryToolProlog<alloc_flags>(new_size)) {
    // Early return if AllocWithMemoryToolProlog returns false
    return nullptr;
  }
  void* result = realloc(ptr, new_size);
  if constexpr (!ContainsFlags(alloc_flags, AllocFlags::kReturnNull)) {
    PA_CHECK(result);
  }
  return result;
#else
  if (PA_UNLIKELY(!ptr)) {
    return AllocInternal<alloc_flags>(new_size, internal::PartitionPageSize(),
                                      type_name);
  }

  if (PA_UNLIKELY(!new_size)) {
    FreeInUnknownRoot<free_flags>(ptr);
    return nullptr;
  }

  if (new_size > internal::MaxDirectMapped()) {
    if constexpr (ContainsFlags(alloc_flags, AllocFlags::kReturnNull)) {
      return nullptr;
    }
    internal::PartitionExcessiveAllocationSize(new_size);
  }

  constexpr bool no_hooks = ContainsFlags(alloc_flags, AllocFlags::kNoHooks);
  const bool hooks_enabled = PartitionAllocHooks::AreHooksEnabled();
  bool overridden = false;
  size_t old_usable_size;
  if (PA_UNLIKELY(!no_hooks && hooks_enabled)) {
    overridden = PartitionAllocHooks::ReallocOverrideHookIfEnabled(
        &old_usable_size, ptr);
  }
  if (PA_LIKELY(!overridden)) {
    // |ptr| may have been allocated in another root.
    SlotSpanMetadata* slot_span = SlotSpanMetadata::FromObject(ptr);
    auto* old_root = PartitionRoot::FromSlotSpanMetadata(slot_span);
    bool success = false;
    bool tried_in_place_for_direct_map = false;
    {
      ::partition_alloc::internal::ScopedGuard guard{
          internal::PartitionRootLock(old_root)};
      // TODO(crbug.com/1257655): See if we can afford to make this a CHECK.
      DCheckIsValidSlotSpan(slot_span);
      old_usable_size = old_root->GetSlotUsableSize(slot_span);

      if (PA_UNLIKELY(slot_span->bucket->is_direct_mapped())) {
        tried_in_place_for_direct_map = true;
        // We may be able to perform the realloc in place by changing the
        // accessibility of memory pages and, if reducing the size, decommitting
        // them.
        success = old_root->TryReallocInPlaceForDirectMap(slot_span, new_size);
      }
    }
    if (success) {
      if (PA_UNLIKELY(!no_hooks && hooks_enabled)) {
        PartitionAllocHooks::ReallocObserverHookIfEnabled(
            CreateFreeNotificationData(ptr),
            CreateAllocationNotificationData(ptr, new_size, type_name));
      }
      return ptr;
    }

    if (PA_LIKELY(!tried_in_place_for_direct_map)) {
      if (old_root->TryReallocInPlaceForNormalBuckets(ptr, slot_span,
                                                      new_size)) {
        return ptr;
      }
    }
  }

  // This realloc cannot be resized in-place. Sadness.
  void* ret = AllocInternal<alloc_flags>(
      new_size, internal::PartitionPageSize(), type_name);
  if (!ret) {
    if constexpr (ContainsFlags(alloc_flags, AllocFlags::kReturnNull)) {
      return nullptr;
    }
    internal::PartitionExcessiveAllocationSize(new_size);
  }

  memcpy(ret, ptr, std::min(old_usable_size, new_size));
  FreeInUnknownRoot<free_flags>(
      ptr);  // Implicitly protects the old ptr on MTE systems.
  return ret;
#endif
}

// Return the capacity of the underlying slot (adjusted for extras) that'd be
// used to satisfy a request of |size|. This doesn't mean this capacity would be
// readily available. It merely means that if an allocation happened with that
// returned value, it'd use the same amount of underlying memory as the
// allocation with |size|.
PA_ALWAYS_INLINE size_t
PartitionRoot::AllocationCapacityFromRequestedSize(size_t size) const {
#if defined(MEMORY_TOOL_REPLACES_ALLOCATOR)
  return size;
#else
  PA_DCHECK(PartitionRoot::initialized);
  size = AdjustSizeForExtrasAdd(size);
  auto& bucket = bucket_at(SizeToBucketIndex(size, GetBucketDistribution()));
  PA_DCHECK(!bucket.slot_size || bucket.slot_size >= size);
  PA_DCHECK(!(bucket.slot_size % internal::kSmallestBucket));

  if (PA_LIKELY(!bucket.is_direct_mapped())) {
    size = bucket.slot_size;
  } else if (size > internal::MaxDirectMapped()) {
    // Too large to allocate => return the size unchanged.
  } else {
    size = GetDirectMapSlotSize(size);
  }
  size = AdjustSizeForExtrasSubtract(size);
  return size;
#endif
}

ThreadCache* PartitionRoot::GetOrCreateThreadCache() {
  ThreadCache* thread_cache = nullptr;
  if (PA_LIKELY(settings.with_thread_cache)) {
    thread_cache = ThreadCache::Get();
    if (PA_UNLIKELY(!ThreadCache::IsValid(thread_cache))) {
      thread_cache = MaybeInitThreadCache();
    }
  }
  return thread_cache;
}

ThreadCache* PartitionRoot::GetThreadCache() {
  return PA_LIKELY(settings.with_thread_cache) ? ThreadCache::Get() : nullptr;
}

// private.
internal::LightweightQuarantineBranch&
PartitionRoot::GetSchedulerLoopQuarantineBranch() {
  ThreadCache* thread_cache = GetThreadCache();
  if (PA_LIKELY(ThreadCache::IsValid(thread_cache))) {
    return thread_cache->GetSchedulerLoopQuarantineBranch();
  } else {
    return *scheduler_loop_quarantine->get();
  }
}

// Explicitly declare common template instantiations to reduce compile time.
#define EXPORT_TEMPLATE                       \
  extern template PA_EXPORT_TEMPLATE_DECLARE( \
      PA_COMPONENT_EXPORT(PARTITION_ALLOC))
EXPORT_TEMPLATE void* PartitionRoot::Alloc<AllocFlags::kNone>(size_t,
                                                              const char*);
EXPORT_TEMPLATE void* PartitionRoot::Alloc<AllocFlags::kReturnNull>(
    size_t,
    const char*);
EXPORT_TEMPLATE void*
PartitionRoot::Realloc<AllocFlags::kNone, FreeFlags::kNone>(void*,
                                                            size_t,
                                                            const char*);
EXPORT_TEMPLATE void*
PartitionRoot::Realloc<AllocFlags::kReturnNull, FreeFlags::kNone>(void*,
                                                                  size_t,
                                                                  const char*);
EXPORT_TEMPLATE void* PartitionRoot::AlignedAlloc<AllocFlags::kNone>(size_t,
                                                                     size_t);
#undef EXPORT_TEMPLATE

#if BUILDFLAG(ENABLE_BACKUP_REF_PTR_SUPPORT)
// Usage in `raw_ptr_backup_ref_impl.cc` is notable enough to merit a
// non-internal alias.
using ::partition_alloc::internal::PartitionAllocGetSlotStartAndSizeInBRPPool;
#endif  // BUILDFLAG(ENABLE_BACKUP_REF_PTR_SUPPORT)

#if BUILDFLAG(IS_APPLE) && BUILDFLAG(USE_PARTITION_ALLOC_AS_MALLOC)
PA_COMPONENT_EXPORT(PARTITION_ALLOC)
void PartitionAllocMallocHookOnBeforeForkInParent();
PA_COMPONENT_EXPORT(PARTITION_ALLOC)
void PartitionAllocMallocHookOnAfterForkInParent();
PA_COMPONENT_EXPORT(PARTITION_ALLOC)
void PartitionAllocMallocHookOnAfterForkInChild();
#endif  // BUILDFLAG(IS_APPLE) && BUILDFLAG(USE_PARTITION_ALLOC_AS_MALLOC)

}  // namespace partition_alloc

#endif  // BASE_ALLOCATOR_PARTITION_ALLOCATOR_SRC_PARTITION_ALLOC_PARTITION_ROOT_H_
