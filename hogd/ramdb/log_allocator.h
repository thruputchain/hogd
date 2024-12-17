// log_allocator.h - log-based memory allocation
//
// A log-based memory allocator reduces memory fragmentation by storing items
// contiguously instead of placing them in different segments based on their
// size. It is a moving allocator that compacts items to reclaim space.
// Therefore, only a single pointer can be maintained to each item and these
// pointers must be updated during compaction. Compaction is done in
// coordination with a ScalableMap, a type of hash table that the log compactor
// uses to sychronize updates to item locations.
//
// One downside to log-based allocators is that they have to move more bytes
// when items are large. Therefore, we maintain a separate non-moving region to
// store large items so they can be referenced indirectly in the log.
//
// The log is divided into several fixed-sized segments called loglets. Each
// loglet contains contiguous memory of variably-sized key-value pairs. Each
// loglet can be independently and concurrently compacted.
//
// Loglets have the following memory layout:
// [Loglet: ItemHeader:key:value ItemHeader:key:value ...] ...
//
// This design is inspired by the paper "Log-structured Memory for DRAM-based
// Storage" at Fast'14.

#pragma once

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <span>
#include <type_traits>
#include <vector>

#include "hogd/base/bits.h"
#include "hogd/base/perthread.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/ramdb/constants.h"

namespace hogd {

class ScalableMap;

// These poison values are used to fill unallocated memory so that invalid
// accesses to them might be recognizable during debugging.
constexpr inline char kInitPoison = 0xAA;
constexpr inline char kDeletePoison = 0xBB;

// ItemStorage specifies how and if the item is stored
enum class ItemStorage : uint8_t {
  kNormal,    // a normal key-value pair
  kBig,       // a large key-value pair that is stored in a separate region
  kTombstone, // a totem for a dead item
};

// ItemHeader is a header that is placed before each item directly inside a
// loglet's memory segment.
struct ItemHeader {
  std::atomic<ItemStorage> storage;
  uint8_t type;
  uint16_t key_len;
  uint32_t value_len;
  ItemHeader *next{nullptr};

  ItemHeader(uint8_t type, uint16_t key_len, uint32_t value_len)
    : storage(ItemStorage::kNormal), type(type), key_len(key_len),
      value_len(value_len)
  {}
  ~ItemHeader() = default;

  // kv_bytes returns the size of the item in bytes (not including the
  // header).
  size_t kv_bytes() const
  {
    return static_cast<size_t>(key_len) + static_cast<size_t>(value_len);
  }

  // total_bytes returns the size of the item in bytes including the header.
  size_t total_bytes() const
  {
    return align_up(sizeof(*this) + kv_bytes(), alignof(ItemHeader));
  }

  // get_key returns a span of the key data.
  std::span<std::byte> get_key()
  {
    auto *buf = reinterpret_cast<std::byte *>(this) + sizeof(*this);
    return {buf, key_len};
  }

  // get_value returns a span of the value data.
  std::span<std::byte> get_value()
  {
    auto *buf = reinterpret_cast<std::byte *>(this) + sizeof(*this) + key_len;
    return {buf, value_len};
  }

  // poison sets the item's memory to a known invalid value (for debugging)
  void poison()
  {
    auto *buf = reinterpret_cast<char *>(this) + sizeof(*this);
    std::memset(buf, kDeletePoison, kv_bytes());
  }
};

// ItemHeader must have a trivial destructor that performs no actions.
static_assert(std::is_trivially_destructible_v<ItemHeader>);

namespace detail {

// Loglet manages one log-structured memory segment.
class Loglet {
public:
  class Iterator {
  public:
    using difference_type = ptrdiff_t;
    using value_type = ItemHeader;
    using pointer = ItemHeader *;
    using reference = ItemHeader &;

    Iterator() = default;
    Iterator(pointer ptr) : pos_(ptr) {}

    reference operator*() const { return *pos_; }
    pointer operator->() const { return pos_; }

    Iterator &operator++()
    {
      pos_ = next_item();
      return *this;
    }
    Iterator operator++(int)
    {
      Iterator tmp = *this;
      ++*this;
      return tmp;
    }

    friend bool operator==(const Iterator &lhs, const Iterator &rhs)
    {
      return lhs.pos_ == rhs.pos_;
    }

  private:
    ItemHeader *pos_;

    ItemHeader *next_item() const
    {
      auto *addr = reinterpret_cast<char *>(pos_);
      addr += align_up(sizeof(*pos_) + pos_->kv_bytes(), alignof(ItemHeader));
      return reinterpret_cast<ItemHeader *>(addr);
    }
  };
  static_assert(std::forward_iterator<Iterator>);

  Loglet() = default;
  ~Loglet() = default;

  // Disable move and copy.
  Loglet(Loglet &&ll) = delete;
  Loglet &operator=(Loglet &&ll) = delete;
  Loglet(const Loglet &ll) = delete;
  Loglet &operator=(const Loglet &ll) = delete;

  // iterator support.
  Iterator begin() const
  {
    assert(is_compactable());
    return Iterator(reinterpret_cast<ItemHeader *>(start_));
  }
  Iterator end() const
  {
    assert(is_compactable());
    return Iterator(reinterpret_cast<ItemHeader *>(start_ + pos_));
  }

  // is_attached returns true if the loglet is ready for use. It is undefined
  // behavor to use the loglet (e.g., allocate, reallocate, or deallocate)
  // before it has been attached.
  bool is_attached() const { return start_ != nullptr; }

  // is_compactable returns true if the loglet can be compacted.
  bool is_compactable() const
  {
    return compactable_.load(std::memory_order_acquire);
  }

  // set_compactable marks the loglet as compactable.
  void set_compactable()
  {
    compactable_.store(true, std::memory_order_release);
  }

  // attach sets the loget's segment location. It must be called before using
  // the loglet.
  void attach(void *start, size_t len)
  {
    start_ = reinterpret_cast<char *>(start);
    len_ = len;
  }

  // allocate creates an item on the loglet. It returns the item, or nullptr
  // if there is insufficient space.
  ItemHeader *allocate(ItemStorage storage, uint8_t type, size_t key_len,
                       size_t value_len);

  // deallocate destroys an item on the loglet. It doesn't actually deallocate
  // the item.  Instead, it marks it with a tombstone so it can be compacted
  // later.
  void deallocate(ItemHeader *item);

  // reset erases all the items and makes the loglet empty.
  void reset()
  {
    free_len_ = 0;
    pos_ = 0;
    compactable_ = false;
  }

  // dont_need releases the backing pages. They will be zeroed when the OS
  // faults them back in.
  void dont_need();

  // poison sets the loglet's segment to a known invalid value (for debugging)
  void poison() const { std::memset(start_, kInitPoison, len_); }

  // bytes_left returns the bytes that can still be allocated in the loglet.
  size_t bytes_left() const { return len_ - pos_ - sizeof(ItemHeader); }

  // live_bytes returns the bytes that are currently allocated toward
  // items, excluding dead items.
  size_t live_bytes() const
  {
    return pos_ - free_len_.load(std::memory_order_acquire);
  }

  // fully_compacted returns true if all allocated bytes are live.
  bool fully_compacted() const
  {
    return free_len_.load(std::memory_order_acquire) == 0;
  }

  // utilization returns the fraction of bytes that are not being wasted due to
  // fragmentation. A higher fraction indicates that a smaller amount of memory
  // can be recovered by compacting the loglet.
  float utilization() const
  {
    return static_cast<float>(live_bytes()) / static_cast<float>(len_);
  }

private:
  // the start of the loglet's memory segment
  char *start_{nullptr};
  // the length of the loglet's memory segment
  size_t len_{0};
  // the number of bytes that are unused (fragmented) below pos_
  std::atomic<size_t> free_len_{0};
  // the number of bytes that are allocated so far (must be <= len_)
  size_t pos_{0};
  // if true, the loglet is not being actively used and can be compacted.
  std::atomic_bool compactable_{false};

  // item_is_within returns true if the item fully resides in this loglet's
  // segment.
  bool item_is_within(ItemHeader *item) const
  {
    auto *pos = reinterpret_cast<char *>(item);
    return pos >= start_ &&
           pos + sizeof(*item) + item->kv_bytes() <= start_ + len_;
  }
};

// TLLogletHead manages a per-thread pointer to a loglet. It provides cleanup
// when a thread that is using it exits.
//
// Usage:
// Perthread<TLLogletHead> head;
struct TLLogletHead {
  Loglet *ll{nullptr};

  TLLogletHead() = default;
  ~TLLogletHead()
  {
    if (ll)
      ll->set_compactable();
  }
};

// CompactorStats stores statistics about log compaction.
struct CompactionStats {
  size_t items;
  size_t live_items;
  size_t loglets_freed;

  CompactionStats &operator+=(const CompactionStats &stats)
  {
    items += stats.items;
    live_items += stats.live_items;
    loglets_freed += stats.loglets_freed;
    return *this;
  }
};

} // namespace detail

// LogAllocator provides a log-based memory allocator for key-value pairs.
class LogAllocator {
public:
  // create is a factory method to construct a log allocator.
  static std::unique_ptr<LogAllocator>
  create(size_t len, ScalableMap *map, WorkerPool &pool,
         std::function<void()> low_mem_observer);

  LogAllocator(void *base, size_t len, WorkerPool &pool,
               std::function<void()> low_mem_fn)
    : base_addr_(base), len_(len), reserved_count_(pool.worker_count()),
      loglets_(len / kLogletRegionSize), pool_(pool),
      low_mem_observer_(std::move(low_mem_fn))
  {}
  ~LogAllocator();

  // Disable move and copy.
  LogAllocator(LogAllocator &&la) = delete;
  LogAllocator &operator=(LogAllocator &la) = delete;
  LogAllocator(const LogAllocator &la) = delete;
  LogAllocator &operator=(const LogAllocator &la) = delete;

  // allocate creates an item.
  // WARNING: Can block if waiting for compaction.
  ItemHeader *allocate(uint8_t type, size_t key_len, size_t value_len);

  // deallocate destroys an item.
  void deallocate(ItemHeader *item);

  // compact initiates compaction to reclaim memory from dead items.
  void compact();

  // print_stats prints memory usage and fragmentation statistics.
  void print_stats();

private:
  using Loglet = detail::Loglet;
  using CompactionStats = detail::CompactionStats;

  // the start of the region where loglets can be mapped.
  void *const base_addr_;
  // the length of the region where loglets can be mapped.
  const size_t len_;
  // the number of loglets that must be reserved.
  const int reserved_count_;
  // a vector of all loglets in the order they appear in the region.
  std::vector<Loglet> loglets_;
  // the map the items are stored in. Needed for compaction.
  ScalableMap *map_;

  // a lock to protect the management of loglets (all the below).
  std::mutex lock_;
  // a vector of free (empty) loglets.
  std::vector<Loglet *> free_loglets_;
  // wakes allot_loglet when it is waiting for more loglets.
  std::condition_variable allot_cv_;
  // out of memory. If set, this causes allocate() to give up.
  bool oom_{false};

  // this worker pool is used for compaction work.
  WorkerPool &pool_;
  // this function is called to notify when compaction is needed.
  std::function<void()> low_mem_observer_;
  // each thread gets its own loglet head.
  PerThread<detail::TLLogletHead> head_;

  // item_is_within returns true if the item fully resides in this log's
  // segment.
  bool item_is_within(ItemHeader *item) const
  {
    auto start = reinterpret_cast<uintptr_t>(base_addr_);
    uintptr_t end = start + len_;
    auto pos = reinterpret_cast<uintptr_t>(item);
    return pos >= start && pos + sizeof(*item) + item->kv_bytes() <= end;
  }

  // get_loglet returns the loget the item is stored in.
  Loglet &get_loglet(ItemHeader *item)
  {
    assert(item_is_within(item));
    off_t offset = reinterpret_cast<uintptr_t>(item) -
                   reinterpret_cast<uintptr_t>(base_addr_);
    return loglets_[offset / kLogletRegionSize];
  }

  // get_loglet_head returns a loglet to allocate a new item into. It will have
  // at least kv_len bytes available.
  inline Loglet *get_loglet_head(size_t kv_len);

  // allot_loglet returns an empty loglet. If use_reserved is true, it cannot
  // fail and will not block because it draws from a pool of loglets reserved
  // for compaction. Otherwise, it could return nullptr if compaction failed.
  Loglet *allot_loglet(bool use_reserved = false);

  // compaction_needed returns true if the compactor should run.
  bool compaction_needed() const;

  // do_compact is an internal method that performs the actual compaction.
  // Loglets with utilization below min_utilization will be compacted. Using a
  // larger fraction will reclaim more memory at the cost of more CPU time.
  // Returns the bytes that were reclaimed.
  size_t do_compact(float min_utilization);

  // compact_one is a helper function that compacts a single loglet.
  CompactionStats compact_one(Loglet &src);
};

} // namespace hogd
