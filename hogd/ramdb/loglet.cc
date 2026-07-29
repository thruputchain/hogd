#include "hogd/ramdb/loglet.h"

extern "C" {
#include <sys/mman.h>
}

#include <algorithm>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <limits> // NOLINT(misc-include-cleaner)
#include <memory>
#include <mutex>
#include <utility>

#include "hogd/base/bits.h"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/status.h"
#include "hogd/base/string.h"
#include "hogd/base/types.h"
#include "hogd/ramdb/constants.h"

namespace hogd {

namespace {

inline void *
map_segment(size_t len)
{
  constexpr int kPermissions = PROT_READ | PROT_WRITE;
  constexpr int kFlags = MAP_PRIVATE | MAP_ANONYMOUS;

  void *addr;
#ifdef __linux__
  // first try a huge page mapping for better performance.
  addr = mmap(nullptr, len, kPermissions, kFlags | MAP_HUGETLB, -1, 0);
  if (addr != MAP_FAILED)
    return addr;

  warn_first_n(1, "Couldn't mmap huge pages: {}.", Errno());
  info_first_n(1, "Consider reserving huge pages to improve performance");
#endif // defined(__linux__)

  // then fallback to a standard mapping.
  addr = mmap(nullptr, len, kPermissions, kFlags, -1, 0);
  if (addr == MAP_FAILED) {
    Errno e;
    error("Couldn't mmap segment: {}", e);
    e.raise();
  }
  return addr;
}

inline void
unmap_segment(void *addr, size_t len)
{
  if (munmap(addr, len) == -1)
    error("Couldn't munmap segment: {}", Errno());
}

inline void
dont_need_segment(void *addr, size_t len)
{
  // Previously we used DONTNEED here, but with DONTNEED there doesn't seem to
  // be a defined way to synchronize with the kernel zeroer. After we've started
  // reusing a page, we've seen the kernel zero the page.
  // MADV_FREE avoids this by clearing the dirty bit on the page and then
  // canceling the any replacement/zeroing of the page if the dirty bit gets
  // reset.
  if (madvise(addr, len, MADV_FREE) == -1) {
    warn("madvise w/ MADV_FREE failed: {}", Errno());
    info("madvise {} {}", addr, len);
  }
}

} // namespace

ItemHeader *
Loglet::alloc(ItemStorage storage, uint8_t type, size_t key_len,
              size_t value_len)
{
  assert(is_attached());
  assert(key_len <= std::numeric_limits<decltype(ItemHeader::key_len)>::max());
  assert(value_len <=
         std::numeric_limits<decltype(ItemHeader::value_len)>::max());

  // check if there is enough space.
  size_t len = sizeof(ItemHeader) + key_len + value_len;
  len = align_up(len, alignof(ItemHeader));
  if (unlikely(pos_ + len >= len_))
    return nullptr;

  // initialize and return the item header.
  auto *h = new (start_ + pos_) ItemHeader(storage, type, key_len, value_len);
  assert(item_is_within(h));
  pos_ += len;
  return h;
}

ItemHeader *
Loglet::clone(ItemHeader *src)
{
  assert(is_attached());

  // check if there is enough space.
  size_t len = sizeof(ItemHeader) + src->key_len + src->value_len;
  len = align_up(len, alignof(ItemHeader));
  if (unlikely(pos_ + len >= len_))
    return nullptr;

  // copy and return the new item header.
  auto *dst = new (start_ + pos_) ItemHeader(*src);
  assert(item_is_within(dst));
  pos_ += len;
  return dst;
}

void
Loglet::dont_need()
{
  assert(is_attached());

  dont_need_segment(start_, len_);
}

std::unique_ptr<LogletAllocator>
LogletAllocator::create(size_t len, size_t reserved_count,
                        std::function<void()> low_mem_observer)
{
  len = align_up(len, kLogletRegionSize);
  void *raw = map_segment(len);
  // Start the lifetime of a std::byte[len] array over the raw mmap region.
  // This anchors every later start_+offset / base_addr_+offset operation as
  // well-defined byte-pointer arithmetic within one C++ array object instead
  // of implementation-defined pointer-integer manipulation over a raw POSIX
  // mapping. (P0593R6's std::start_lifetime_as_array is the C++23 primitive
  // for this but isn't shipped in our libstdc++/libc++ yet; placement array-
  // new of a trivial implicit-lifetime type is the portable equivalent and
  // emits no array cookie.)
  auto *base = new (raw) std::byte[len];
  auto log = std::make_unique<LogletAllocator>(base, len, reserved_count,
                                               std::move(low_mem_observer));
  std::byte *pos = base;
  for (Loglet &l : log->loglets_) {
    l.attach(pos, kLogletRegionSize);
    pos += kLogletRegionSize;
    log->free_loglets_.push_back(&l);
  }
  log->min_free_loglets_ = log->not_needed_loglets_ = log->free_loglets_.size();
  return log;
}

LogletAllocator::~LogletAllocator()
{
  unmap_segment(base_addr_, len_);
}

void
LogletAllocator::low_mem_done(bool success)
{
  std::unique_lock ul(lock_);

  // if reclamation was unsuccessful, trigger OOM failures.
  if (free_loglets_.size() <= static_cast<size_t>(reserved_count_) &&
      !success) {
    error("out of loglet memory; allocations will now fail!");
    oom_ = true;
    alloc_cv_.notify_all();
  }

  // retrigger memory reclamation if still low.
  if (mem_is_low()) {
    ul.unlock();
    low_mem_observer_();
    return;
  }

  // otherwise allow low mem to be triggered in the future.
  low_mem_in_service_ = false;
}

Loglet *
LogletAllocator::alloc(LogletMode mode, bool use_reserved)
{
  assert(mode != LogletMode::kFree);

  std::unique_lock ul(lock_);

  // provide low memory notifications (allow only one in flight at a time).
  if (!low_mem_in_service_ && mem_is_low()) {
    low_mem_in_service_ = true;
    ul.unlock();
    low_mem_observer_();
    ul.lock();
  }

  // wait for enough loglets to become available.
  if (!use_reserved) {
    alloc_cv_.wait(ul, [this] {
      return free_loglets_.size() > static_cast<size_t>(reserved_count_) ||
             oom_;
    });
    if (unlikely(oom_))
      return nullptr;
  }

  // allocate a free loglet.
  //
  // The reserved path skips the wait above, so the free list can legitimately
  // be empty here: sustained compaction draws reserved loglets faster than
  // finishing compactors return their sources. Fail closed and let the caller
  // cope -- make_unique_loglet() already documents that its result must be
  // checked. Asserting instead left Release builds reading back() off an empty
  // vector.
  if (unlikely(free_loglets_.empty()))
    return nullptr;
  Loglet *ll = free_loglets_.back();
  free_loglets_.pop_back();
  if constexpr (is_debug_build())
    ll->poison();
  ll->mode_.store(mode, std::memory_order_relaxed);
  min_free_loglets_ = std::min(min_free_loglets_, free_loglets_.size());
  return ll;
}

void
LogletAllocator::free(Loglet *ll)
{
  // reset the loglet to empty.
  ll->reset();

  // add the loglet to the free list.
  {
    std::lock_guard guard(lock_);
    oom_ = false;
    free_loglets_.push_back(ll);
    if (free_loglets_.size() > static_cast<size_t>(reserved_count_))
      alloc_cv_.notify_one();
  }
}

void
LogletAllocator::print_stats()
{
  size_t disk = 0, mem = 0, free = 0;
  for (const Loglet &ll : loglets_) {
    using enum LogletMode;
    switch (ll.get_usage_mode()) {
    case kMemoryItems:
      mem++;
      break;
    case kDiskBuffer:
      disk++;
      break;
    default:
      free++;
    }
  }

  size_t total = mem + disk + free;
  info("mem stats: {} ({}% memory items, {}% disk buffering, {}% free)",
       print_iec_bytes(len_), mem * 100 / total, disk * 100 / total,
       free * 100 / total);
}

void
LogletAllocator::reclaim_idle_memory()
{
  // Hold lock_ across the per-loglet madvise too. Dropping it mid-sweep (as we
  // used to) let alloc() hand out a sampled loglet and a writer publish a live
  // item into it before the now-stale MADV_FREE ran, discarding live data
  // (#2023). It's a cheap, low-frequency background pass.
  std::lock_guard guard(lock_);

  // not_needed_loglets_ may exceed the free-list size after alloc() drained it
  // below the watermark; the i < min_free_loglets_ guard makes that a no-op.
  assert(min_free_loglets_ <= free_loglets_.size());

  if (min_free_loglets_ > not_needed_loglets_)
    debug("giving back {} loglets to the OS",
          min_free_loglets_ - not_needed_loglets_);

  for (size_t i = not_needed_loglets_; i < min_free_loglets_; ++i) {
    Loglet *ll = free_loglets_[i];
    CHECK(ll->get_usage_mode() == LogletMode::kFree);
    ll->dont_need();
  }

  not_needed_loglets_ = min_free_loglets_;
  min_free_loglets_ = free_loglets_.size();
}

} // namespace hogd
