#include "hogd/ramdb/log_allocator.h"

extern "C" {
#include <sys/mman.h>
}

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "hogd/base/bits.h"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/status.h"
#include "hogd/base/string.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/ramdb/constants.h"
#include "hogd/ramdb/scalable_map.h"

namespace hogd {

namespace {

void *
map_segment(size_t len)
{
  constexpr int kPermissions = PROT_READ | PROT_WRITE;
  constexpr int kFlags = MAP_PRIVATE | MAP_ANONYMOUS;

  // first try a huge page mapping for better performance.
  void *addr = mmap(nullptr, len, kPermissions, kFlags | MAP_HUGETLB, -1, 0);
  if (addr != MAP_FAILED)
    return addr;

  warn_first_n(1, "Couldn't mmap huge pages: {}.", Errno());
  info_first_n(1, "Consider reserving huge pages to improve performance");

  // then fallback to a standard mapping.
  addr = mmap(nullptr, len, kPermissions, kFlags, -1, 0);
  if (addr == MAP_FAILED) {
    Errno e;
    error("Couldn't mmap segment: {}", e);
    e.raise();
  }
  return addr;
}

void
unmap_segment(void *addr, size_t len)
{
  if (munmap(addr, len) == -1)
    error("Couldn't munmap segment: {}", Errno());
}

void
dont_need_segment(void *addr, size_t len)
{
  if (madvise(addr, len, MADV_DONTNEED) == -1)
    warn("madvise w/ MADV_DONTNEED failed: {}", Errno());
}

} // namespace

namespace detail {

ItemHeader *
Loglet::allocate(ItemStorage storage, uint8_t type, size_t key_len,
                 size_t value_len)
{
  assert(is_attached());
  assert(key_len <= std::numeric_limits<decltype(ItemHeader::key_len)>::max());
  assert(value_len <=
         std::numeric_limits<decltype(ItemHeader::value_len)>::max());
  assert(storage != ItemStorage::kTombstone);

  // check if there is enough space.
  size_t len = sizeof(ItemHeader) + key_len + value_len;
  if (unlikely(pos_ + len >= len_))
    return nullptr;

  // initialize and return the item header.
  auto *h = new (start_ + pos_) ItemHeader(type, key_len, value_len);
  assert(item_is_within(h));
  pos_ += align_up(len, alignof(ItemHeader));
  return h;
}

void
Loglet::deallocate(ItemHeader *item)
{
  assert(is_attached());
  assert(item_is_within(item));
  assert(item->storage != ItemStorage::kTombstone);

  // poison the item's memory if debug build.
  if (is_debug_build())
    item->poison();

  // mark the item free.
  item->storage.store(ItemStorage::kTombstone, std::memory_order_release);
  size_t bytes_fragmented = item->total_bytes();
  free_len_.fetch_add(bytes_fragmented, std::memory_order_release);
}

void
Loglet::dont_need()
{
  assert(is_attached());

  dont_need_segment(start_, len_);
}

} // namespace detail

std::unique_ptr<LogAllocator>
LogAllocator::create(size_t len, ScalableMap *map, WorkerPool &pool,
                     std::function<void()> low_mem_observer)
{
  len = align_up(len, kLogletRegionSize);
  void *addr = map_segment(len);
  auto log = std::make_unique<LogAllocator>(addr, len, pool,
                                            std::move(low_mem_observer));
  auto *pos = reinterpret_cast<char *>(addr);
  for (Loglet &l : log->loglets_) {
    l.attach(pos, kLogletRegionSize);
    pos += kLogletRegionSize;
    log->free_loglets_.push_back(&l);
  }
  log->map_ = map;
  return log;
}

LogAllocator::~LogAllocator()
{
  unmap_segment(base_addr_, len_);
}

detail::Loglet *
LogAllocator::allot_loglet(bool use_reserved)
{
  std::unique_lock lock(lock_);

  // notify compaction observer if memory is low.
  if (compaction_needed())
    low_mem_observer_();

  // wait for a loglet to become available.
  if (!use_reserved) {
    allot_cv_.wait(lock, [this] {
      return free_loglets_.size() > static_cast<size_t>(reserved_count_) ||
             oom_;
    });
    if (unlikely(oom_))
      return nullptr;
  }

  // take a loglet.
  assert(!free_loglets_.empty());
  Loglet *ll = free_loglets_.back();
  if (is_debug_build())
    ll->poison();
  free_loglets_.pop_back();
  return ll;
}

detail::Loglet *
LogAllocator::get_loglet_head(size_t kv_len)
{
  // allocate a the first loglet head (if needed).
  if (unlikely(!head_->ll)) {
    head_->ll = allot_loglet();
    if (!head_->ll)
      return nullptr;
  }

  // try to use the thread's current loglet head.
  if (likely(head_->ll->bytes_left() >= sizeof(ItemHeader) + kv_len))
    return head_->ll;

  // otherwise the loglet head is full, try to allocate a new one.
  head_->ll->set_compactable();
  head_->ll = allot_loglet();
  return head_->ll;
}

ItemHeader *
LogAllocator::allocate(uint8_t type, size_t key_len, size_t value_len)
{
  Loglet *ll = get_loglet_head(key_len + value_len);
  if (unlikely(!ll))
    return nullptr;
  return ll->allocate(ItemStorage::kNormal, type, key_len, value_len);
}

void
LogAllocator::deallocate(ItemHeader *item)
{
  Loglet &ll = get_loglet(item);
  ll.deallocate(item);
}

void
LogAllocator::print_stats()
{
  size_t uncompactable_bytes = 0;
  size_t live_bytes = 0;

  for (const Loglet &ll : loglets_) {
    if (ll.is_compactable()) {
      live_bytes += ll.live_bytes();
      continue;
    }
    uncompactable_bytes += kLogletRegionSize;
  }

  size_t free_bytes;
  {
    std::lock_guard guard(lock_);
    free_bytes = free_loglets_.size() * kLogletRegionSize;
  }

  size_t fragmented_bytes = len_ - live_bytes - uncompactable_bytes;
  size_t inuse_bytes = uncompactable_bytes - free_bytes;
  info("mem stats: {} ({}% free, {}% fragmented, {}% live, {}% in_use)",
       print_iec_bytes(len_), free_bytes * 100 / len_,
       fragmented_bytes * 100 / len_, live_bytes * 100 / len_,
       inuse_bytes * 100 / len_);
}

detail::CompactionStats
LogAllocator::compact_one(Loglet &src)
{
  assert(src.is_compactable());
  CompactionStats stats{};
  size_t bytes_moved = 0;

  // move live items and skip dead items to compact.
  for (ItemHeader &h : src) {
    stats.items++;
    ItemHeader *src_item = std::addressof(h);

    // loglet compaction is finished if every live item has been moved.
    if (src.live_bytes() == bytes_moved)
      break;

    // skip dead items that can be detected without locking.
    if (src_item->storage.load(std::memory_order_acquire) ==
        ItemStorage::kTombstone) {
      continue;
    }

    // move the item to the new loglet (if it is still live after holding lock).
    [[maybe_unused]] bool found = map_->modify_locked(
        src_item->get_key(),
        [this, src_item, &bytes_moved, &stats](ItemHeader **hptr) mutable {
          // recheck if item is dead now that the lock is held.
          if (unlikely(src_item->storage.load(std::memory_order_relaxed) ==
                       ItemStorage::kTombstone)) {
            return;
          }

          // initialize the first use of the per-thread loglet.
          static thread_local Loglet *dst;
          if (unlikely(!dst))
            dst = allot_loglet(true);

          // allocate the destination item.
          ItemHeader *dst_item =
              dst->allocate(ItemStorage::kNormal, src_item->type,
                            src_item->key_len, src_item->value_len);
          if (!dst_item) {
            // loglet is full; get a new one.
            dst->set_compactable();
            dst = allot_loglet(true);
            dst_item = dst->allocate(ItemStorage::kNormal, src_item->type,
                                     src_item->key_len, src_item->value_len);
            assert(dst_item != nullptr);
          }

          // copy the item's key and value to the new location.
          std::ranges::copy(src_item->get_key(), dst_item->get_key().begin());
          std::ranges::copy(src_item->get_value(),
                            dst_item->get_value().begin());

          // point the map to the new location (bucket lock is held).
          assert(*hptr == src_item);
          dst_item->next = src_item->next;
          *hptr = dst_item;
          stats.live_items++;
          bytes_moved += dst_item->total_bytes();
        });
    assert(found || src_item->storage == ItemStorage::kTombstone);
  }

  // source loglet can now be reset and freed.
  src.reset();
  src.dont_need();
  {
    std::lock_guard guard(lock_);
    free_loglets_.push_back(&src);
    if (free_loglets_.size() > static_cast<size_t>(reserved_count_)) {
      allot_cv_.notify_one();
      stats.loglets_freed = 1;
    }
  }

  return stats;
}

void
LogAllocator::compact()
{
  info("starting compaction (up to {}% live loglets)",
       kMinCompactUtilization * 100);
  do_compact(kMinCompactUtilization);

  while (true) {
    {
      std::lock_guard guard(lock_);
      if (!compaction_needed())
        break;
    }

    warn("memory is still low after compaction");
    warn("attempting full compaction (up to 100% live loglets)");
    size_t bytes_reclaimed = do_compact(1.0F);
    if (bytes_reclaimed == 0)
      error("Unable to free memory! Allocations will now fail.");

    // notify changes in out of memory (oom) status.
    {
      std::lock_guard guard(lock_);
      bool old_oom = oom_;
      oom_ = bytes_reclaimed == 0;
      if (oom_ && oom_ != old_oom)
        allot_cv_.notify_all();
    }

    // sleep before trying again (if oom), so we don't flood the system with
    // compaction attempts.
    using namespace std::literals::chrono_literals;
    if (oom_)
      std::this_thread::sleep_for(1s); // NOLINT(misc-include-cleaner)
  }
}

inline bool
LogAllocator::compaction_needed() const
{
  // check if below the reserved loglet threshold.
  if (free_loglets_.size() <= static_cast<size_t>(reserved_count_))
    return true;

  // check if below the free memory threshold.
  if (static_cast<float>(free_loglets_.size()) /
          static_cast<float>(loglets_.size()) <=
      kLowMemoryRatio) {
    return true;
  }

  return false;
}

size_t
LogAllocator::do_compact(float min_utilization)
{
  std::vector<std::future<CompactionStats>> completions;
  completions.reserve(loglets_.size());

  // record start time for measuring compaction time.
  using clock = std::chrono::high_resolution_clock;
  clock::time_point start = clock::now();

  // launch compaction on every eligible loglet.
  for (Loglet &ll : loglets_) {
    if (!ll.is_compactable() || ll.fully_compacted())
      continue;
    if (ll.utilization() >= min_utilization)
      continue;

    completions.emplace_back(
        pool_.submit([this, &ll] { return compact_one(ll); }));
  }

  // wait for compactions to finish.
  CompactionStats stats{};
  for (std::future<CompactionStats> &f : completions)
    stats += f.get();

  // print the compaction rate.
  clock::time_point end = clock::now();
  auto seconds = std::chrono::duration<double>(end - start).count();
  info("finished compaction in {:.3f} seconds", seconds);
  if (stats.items > 0) {
    info("scanned {:.0f} items/sec ({}% live)",
         static_cast<double>(stats.items) / seconds,
         stats.live_items * 100 / stats.items);
  }
  else {
    warn("no items scanned.");
  }

  // determine how much memory was freed.
  size_t free_bytes = stats.loglets_freed * kLogletRegionSize;
  info("reclaimed {}/{} ({}% freed)", print_iec_bytes(free_bytes),
       print_iec_bytes(len_), free_bytes * 100 / len_);
  print_stats();
  return free_bytes;
}

} // namespace hogd
