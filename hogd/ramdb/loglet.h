// loglet.h - support for loglet allocation.
//
// Loglets are fixed-sized memory segments (like pages) used by ramdb for item
// allocation and buffering to disk. Each loglet contains a log of changes and
// acts like a bump allocator internally.
//
// Loglets are allocated with a free list that carves up a big contiguous
// mapping. The mapping has the following memory layout:
// [Loglet: ItemHeader:key:value ItemHeader:key:value ...] ...

#pragma once

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <exception>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "hogd/base/bits.h"
#include "hogd/base/log.h"
#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/ramdb/constants.h"

namespace hogd {

// forward declaration.
class LogletAllocator;

// This poison value is used to fill unallocated memory in loglets so that
// invalid accesses to them are more easy to recognize during debugging.
constexpr inline char kPoisonValue = 0xAA;

// ItemStorage specifies the type of item being stored
enum class ItemStorage : uint8_t {
  kNormal,    // a normal key-value pair
  kBig,       // a large key-value pair that is stored in a separate region
  kTombstone, // a totem for a dead item
  kCommit,    // commit the preceding modifications (on-disk log only)
};

// ItemHeader is a header that is placed before each item directly inside a
// loglet memory segment.
struct ItemHeader {
  ItemStorage storage;
  uint8_t type;
  uint16_t key_len;
  uint32_t value_len;
  uint64_t lsn; // Log Sequence Number
  ItemHeader *next{nullptr};

  ItemHeader() = default;
  ItemHeader(ItemStorage storage, uint8_t type, uint16_t key_len,
             uint32_t value_len)
    : storage(storage), type(type), key_len(key_len), value_len(value_len)
  {}
  ~ItemHeader() = default;

  // kv_bytes returns the size of the item in bytes (not including the
  // header).
  size_t kv_bytes() const
  {
    return static_cast<size_t>(key_len) + static_cast<size_t>(value_len);
  }

  // total_bytes returns the size of the item in bytes including the header.
  size_t total_bytes() const { return sizeof(*this) + kv_bytes(); }

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

  // get_key_and_value returns a contiguous span of the key data and the value
  // data.
  std::span<std::byte> get_key_and_value()
  {
    auto *buf = reinterpret_cast<std::byte *>(this) + sizeof(*this);
    return {buf, kv_bytes()};
  }

  // get_all returns a contiguous span of the key data, the value data, and any
  // alignment padding.
  std::span<std::byte> get_all()
  {
    auto *buf = reinterpret_cast<std::byte *>(this) + sizeof(*this);
    return {buf, align_up(kv_bytes(), alignof(ItemHeader))};
  }
};

// ItemHeader must have a trivial destructor that performs no actions and must
// be trivially copyable.
static_assert(std::is_trivially_destructible_v<ItemHeader>);
static_assert(std::is_trivially_copyable_v<ItemHeader>);

// LogletMode specifies how the loglet is currently being used.
enum class LogletMode : uint8_t {
  kFree,        // the loglet is free and available for allocation
  kDiskBuffer,  // the loglet is buffering writes to disk
  kMemoryItems, // the loglet is holding in-memory items
};

// Loglet manages one log-structured memory segment. Loglets are fixed-sized
// and are used both for the in-memory log and as buffers for writing to the
// on-disk log.
class Loglet {
public:
  friend LogletAllocator;

  // Iterator walks through each ItemHeader in the loglet.
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
    return Iterator(reinterpret_cast<ItemHeader *>(start_));
  }
  Iterator end() const
  {
    return Iterator(reinterpret_cast<ItemHeader *>(start_ + pos_));
  }

  // get_bytes returns a span of all bytes stored in the loglet.
  std::span<const std::byte> get_bytes() const
  {
    return writable_byte_view(start_, pos_);
  }

  // is_attached returns true if the loglet is ready for use. It is undefined
  // behavor to use the loglet (e.g., allocate, reallocate, or deallocate)
  // before it has been attached.
  bool is_attached() const { return start_ != nullptr; }

  // attach sets the loget's segment location. It must be called before using
  // the loglet.
  void attach(void *start, size_t len)
  {
    start_ = reinterpret_cast<char *>(start);
    len_ = len;
  }

  // alloc allocates an item in this loglet. It returns the item header, or
  // nullptr if there is insufficient space.
  ItemHeader *alloc(ItemStorage storage, uint8_t type, size_t key_len,
                    size_t value_len);

  // clone copies an item into this loglet. It reserves space for its key and
  // value. It returns nullptr if there is insufficient space.
  ItemHeader *clone(ItemHeader *src);

  // free removes an item from this loglet. It doesn't directly reclaim the
  // item. Instead, it marks an the existing item with a tombstone so it can be
  // compacted later.
  template<bool Rewind> void free(ItemHeader *item);

  // reset erases all the items and makes the loglet empty.
  void reset()
  {
    free_len_ = 0;
    pos_ = 0;
    mode_.store(LogletMode::kFree, std::memory_order_relaxed);
  }

  // dont_need releases the backing pages. They will be zeroed when the OS
  // faults them back in.
  void dont_need();

  // poison sets the loglet's segment to a known invalid value (for debugging)
  void poison() const { std::memset(start_, kPoisonValue, len_); }

  // bytes_left returns the bytes that can still be allocated in the loglet.
  size_t bytes_left() const { return len_ - pos_ - sizeof(ItemHeader); }

  // fully_compacted returns true if the loglet is full and all allocated bytes
  // are live.
  bool fully_compacted() const
  {
    return len_ - pos_ < sizeof(ItemHeader) + kBigItemThreshold &&
           free_len_.load(std::memory_order_acquire) == 0;
  }

  // get_usage_mode returns the current usage case for the loglet.
  LogletMode get_usage_mode() const
  {
    return mode_.load(std::memory_order_relaxed);
  }

  // size returns the bytes allocated in this loglet.
  size_t size() const { return pos_; }

  // live_size returns the bytes allocated in this loglet, excluding dead items.
  size_t live_size() const
  {
    return pos_ - free_len_.load(std::memory_order_acquire);
  }

  // utilization returns the fraction of bytes that are not being wasted due to
  // fragmentation. A higher fraction indicates that a smaller amount of memory
  // can be recovered by compacting the loglet.
  float utilization() const
  {
    return static_cast<float>(live_size()) / static_cast<float>(len_);
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
  // the current usage mode for the loglet.
  std::atomic<LogletMode> mode_{LogletMode::kFree};

  // item_is_within returns true if the item fully resides in this loglet's
  // segment.
  bool item_is_within(ItemHeader *item) const
  {
    auto *pos = reinterpret_cast<char *>(item);
    return pos >= start_ &&
           pos + sizeof(*item) + item->kv_bytes() <= start_ + len_;
  }
};

template<bool Rewind>
void
Loglet::free(ItemHeader *item)
{
  assert(is_attached());
  assert(item_is_within(item));
  assert(item->storage != ItemStorage::kTombstone);

  // optimization: if this was the last item allocated in the loglet, we can
  // move back pos_ instead of marking it with a tombstone to avoid the
  // fragmentation it would cause.
  //
  // This only is safe if the loglet is not yet given to the compactor.
  if constexpr (Rewind) {
    size_t pos =
        reinterpret_cast<uintptr_t>(item) - reinterpret_cast<uintptr_t>(start_);
    if (pos + align_up(item->total_bytes(), alignof(ItemHeader)) == pos_) {
      pos_ = pos;
      return;
    }
  }

  // mark the item free.
  auto storage = std::atomic_ref<ItemStorage>(item->storage);
  storage.store(ItemStorage::kTombstone, std::memory_order_release);
  size_t bytes_fragmented = item->total_bytes();
  free_len_.fetch_add(bytes_fragmented, std::memory_order_release);
}

// LogletAllocator manages the allocation of loglets.
class LogletAllocator {
public:
  // create is a factory method to construct a log allocator.
  static std::unique_ptr<LogletAllocator>
  create(size_t len, size_t reserved_count,
         std::function<void()> low_mem_observer);

  LogletAllocator(void *base, size_t len, size_t reserved_count,
                  std::function<void()> low_mem_observer)
    : base_addr_(base), len_(len), reserved_count_(reserved_count),
      loglets_(len / kLogletRegionSize),
      low_mem_observer_(std::move(low_mem_observer))
  {}
  ~LogletAllocator();

  // Disable move and copy.
  LogletAllocator(LogletAllocator &&la) = delete;
  LogletAllocator &operator=(LogletAllocator &&la) = delete;
  LogletAllocator(const LogletAllocator &la) = delete;
  LogletAllocator &operator=(const LogletAllocator &la) = delete;

  // alloc allocates a loglet. If use_reserved is true, it can never fail or
  // block because it draws from a pool of loglets reserved for compaction.
  // Otherwise, it could return nullptr if no loglets are available and
  // attempts to free loglets have failed.
  Loglet *alloc(LogletMode mode, bool use_reserved = false);
  // free frees a loglet, allowing it to be reused.
  void free(Loglet *ll);
  // print_stats prints loglet memory usage statistics.
  void print_stats();
  // reclaim_idle_memory returns idle loglets to the OS. It should be called
  // periodically.
  void reclaim_idle_memory();

  // low_mem_done() must be called by low_mem_observer_ when it is finished. If
  // success is false, out of memory errors will be returned to future and
  // pending attempts to allocate loglets.
  void low_mem_done(bool success);

  // get_loglet returns the loget the item is stored in.
  Loglet &get_loglet(ItemHeader *item)
  {
    assert(item_is_within(item));
    off_t offset = reinterpret_cast<uintptr_t>(item) -
                   reinterpret_cast<uintptr_t>(base_addr_);
    return loglets_[offset / kLogletRegionSize];
  }

  // size returns the size in bytes of all loglets together.
  size_t size() const { return len_; }

private:
  // the start of the region where loglets can be mapped.
  void *const base_addr_;
  // the length of the region where loglets can be mapped.
  const size_t len_;
  // the number of loglets that must be reserved so compaction always succeeds.
  const size_t reserved_count_;
  // a vector of all loglets in the order they appear in the region.
  std::vector<Loglet> loglets_;

  // a lock to protect the management of loglets (all the below).
  std::mutex lock_;
  // a vector of free (empty) loglets.
  std::vector<Loglet *> free_loglets_;
  // wakes alloc() when it is waiting for more loglets.
  std::condition_variable alloc_cv_;
  // out of memory. If set, this causes alloc() to give up.
  bool oom_{false};
  // the smallest observed number of free_loglets_
  size_t min_free_loglets_;
  // the number of free loglets that are not backed by the OS.
  size_t not_needed_loglets_;

  // true if low_mem_observer_ is currently triggered.
  bool low_mem_in_service_{false};
  // this function is called to notify when memory is low and loglets must be
  // freed (e.g., via compaction).
  std::function<void()> low_mem_observer_;

  // item_is_within returns true if the item fully resides in the allocator's
  // region.
  bool item_is_within(ItemHeader *item) const
  {
    auto start = reinterpret_cast<uintptr_t>(base_addr_);
    uintptr_t end = start + len_;
    auto pos = reinterpret_cast<uintptr_t>(item);
    return pos >= start && pos + sizeof(*item) + item->kv_bytes() <= end;
  }

  // mem_is_low returns true if memory is low (and should be reclaimed).
  bool mem_is_low() const;
};

inline bool
LogletAllocator::mem_is_low() const
{
  // Check if below the reserved loglet threshold. If so, there are no
  // loglets that can be allocated for normal use, so we need to urgently
  // free more to continue making progress.
  if (free_loglets_.size() <= static_cast<size_t>(reserved_count_))
    return true;

  // Check if below the free memory threshold. If so, memory is too fragmented
  // so it makes sense to compact it.
  if (static_cast<float>(free_loglets_.size()) /
          static_cast<float>(loglets_.size()) <=
      kLowMemoryRatio) {
    return true;
  }

  return false;
}

// UniqueLoglet provides ownership of a Loglet, automatically freeing it.
class UniqueLoglet {
public:
  UniqueLoglet() : allocator_(nullptr), ll_(nullptr) {}
  UniqueLoglet(LogletAllocator &allocator, Loglet *ll)
    : allocator_(&allocator), ll_(ll)
  {}
  ~UniqueLoglet()
  {
    try {
      reset();
    } catch (const std::exception &e) {
      error("Couldn't free UniqueLoglet: {}", e.what());
    }
  }

  // Enable move and disable copy.
  UniqueLoglet(UniqueLoglet &&ul) noexcept
    : allocator_(ul.allocator_), ll_(std::exchange(ul.ll_, nullptr))
  {}
  UniqueLoglet &operator=(UniqueLoglet &&ul) noexcept
  {
    allocator_ = ul.allocator_;
    ll_ = std::exchange(ul.ll_, nullptr);
    return *this;
  }
  UniqueLoglet(const UniqueLoglet &ul) = delete;
  UniqueLoglet &operator=(const UniqueLoglet &ul) = delete;

  Loglet *operator->() const noexcept { return ll_; }

  explicit operator bool() const noexcept { return ll_ != nullptr; }

  Loglet *get() const noexcept { return ll_; }

  typename std::add_lvalue_reference_t<Loglet> operator*() const noexcept
  {
    return *ll_;
  }

  Loglet *release() noexcept { return std::exchange(ll_, nullptr); }

  void reset(Loglet *ptr = nullptr)
  {
    if (ll_)
      allocator_->free(ll_);
    ll_ = ptr;
  }

private:
  LogletAllocator *allocator_;
  Loglet *ll_;
};

// make_unique_loglet allocates a loglet from allocator and returns a
// UniqueLoglet that manages its ownership.
//
// WARNING: You must check the return value to determine if the allocation was
// successful.
//
// Example:
// auto ll = make_unique_loglet(allocator, LogletMode::kMemoryItems);
// if (!ll)
//   // out of memory
[[nodiscard]] inline UniqueLoglet
make_unique_loglet(LogletAllocator &allocator, LogletMode mode,
                   bool use_reserved = false)
{
  return UniqueLoglet(allocator, allocator.alloc(mode, use_reserved));
}

} // namespace hogd
