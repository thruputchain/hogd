// -*-C++-*-

#pragma once

#include "x23/utility.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <ranges>
#include <semaphore>
#include <type_traits>
#include <utility>
#include <vector>

#include <x23/cleanup.h>
#include <x23/cpumap.h>
#include <x23/debug.h>
#include <x23/mvar.h>
#include <x23/vec.h>

namespace x23 {

class IndexAllocator {
public:
  using index_type = size_t;

  ~IndexAllocator()
  {
    // If this fails, you have indices still allocated, but can no
    // longer free them because this object has been destroyed.
    // (Probably means you have a global destruction order bug.)
    x23assert(max_ == 0);
  }

  index_type alloc()
  {
    lk_.acquire();
    Defer _ul([this] { lk_.release(); });
    if (free_.empty())
      return max_++;
    auto ret = free_.back();
    free_.pop_back();
    return ret;
  }
  void free(index_type i)
  {
    lk_.acquire();
    Defer _ul([this] { lk_.release(); });
#if DEBUG
    x23assert(i < max_ && std::ranges::find(free_, i) == free_.end());
#endif // DEBUG
    free_.push_back(i);
    if (free_.size() == max_) {
      free_.clear();
      max_ = 0;
    }
  }

private:
  // Use 8-byte semaphore because 40-byte mutex overflows cache line.
  std::binary_semaphore lk_{1};
  index_type max_{};
  std::vector<index_type> free_;
};
static_assert(sizeof(IndexAllocator) <= hardware_destructive_interference_size);

// Non-thread-safe allocator, must be wrapped by a lock if used by
// more than one thread.  Never returns memory to the system unless
// you call clear() or destroy the AllocCache.  Not returning memory
// to the system should make it okay to memcpy from possibly freed
// memory in most circumstances.
class AllocCache {
public:
  void *allocate(std::size_t n)
  {
    n = round_size(n);
    auto it = std::ranges::lower_bound(freelists_, n, {}, &FreeList::size);
    if (it != freelists_.end() && it->size != n)
      if (void *head = it->head) {
        it->head = next(head);
        return head;
      }
    return ::operator new(n);
  }

  void deallocate(void *ptr, std::size_t n) noexcept
  {
    if (!ptr)
      return;
    n = round_size(n);

    auto it = std::ranges::lower_bound(freelists_, n, {}, &FreeList::size);
    if (it == freelists_.end() || it->size != n)
      it = freelists_.insert(it, FreeList{.size = n, .head = nullptr});

    next(ptr) = it->head;
    it->head = ptr;
  }

  void clear() noexcept
  {
    for (auto &fl : freelists_) {
      for (void *p = fl.head, *np; p; p = np) {
        np = next(p);
        ::operator delete(p);
      }
      fl.head = nullptr;
    }
    freelists_.clear();
  }

  ~AllocCache() { clear(); }

private:
  struct FreeList {
    std::size_t size;
    void *head; // singly-linked list
  };

  std::vector<FreeList> freelists_; // sorted by size

  static constexpr std::size_t round_size(std::size_t n) noexcept
  {
    return std::max(n, sizeof(void *));
  }
  static void *&next(void *ptr) noexcept { return *static_cast<void **>(ptr); }
};

extern PerCpuSet<MVar<AllocCache>> memcaches;

// A "non-movable vector", can contain up to about 30 GiB of items T
// indexed by an integer.  Items are never moved, so pointers and
// references to items remain valid until the NMVec is resized below
// their index or destroyed.
template<typename T> class NMVec {
  struct Location;

public:
  using value_type = T;
  using difference_type = ptrdiff_t;
  class iterator_type : public Location {
  public:
    using iterator_concept = std::random_access_iterator_tag;
    using value_type = T;
    using difference_type = ptrdiff_t;

    constexpr iterator_type() noexcept = default;
    constexpr explicit iterator_type(NMVec *v, size_t index) noexcept
      : Location(Location::from_index(index)), v_(v) {}
    decltype(auto) operator*() const noexcept
    {
      return v_->get_location(this->loc());
    }
    decltype(auto) operator->() const noexcept
    {
      return std::addressof(v_->get_location(this->loc()));
    }
    decltype(auto) operator[](difference_type n) const noexcept
    {
      return *(*this + n);
    }

    friend bool operator==(const iterator_type &a, const iterator_type &b)
    {
      return a.v_ == b.v_ && a.loc() == b.loc();
    }

#ifndef NDEBUG
    // It's UB to compare iterators from different NMVecs
    void check_compat(const iterator_type &other) const
    {
      x23assert(v_ == other.v_);
    }
#endif // !NDEBUG
  private:
    NMVec *v_{};
  };

  NMVec() noexcept = default;
  NMVec(NMVec &&other) noexcept
  {
    for (unsigned i = 0; i < kNumChunks; ++i)
      chunks_[i].store(other.chunks_[i].exchange(nullptr,
                                                 std::memory_order_relaxed),
                       std::memory_order_relaxed);
    size_.store(other.size_.exchange(0, std::memory_order_relaxed),
                std::memory_order_release);
  }
  ~NMVec() { resize(0); }

  // Not thread-safe
  NMVec &operator=(NMVec &&other) noexcept
  {
    if (&other == this)
      return *this;
    for (unsigned i = 0; i < kNumChunks; ++i)
      if (T *c = chunks_[i].exchange(
              other.chunks_[i].exchange(nullptr, std::memory_order_relaxed),
              std::memory_order_relaxed))
        a.deallocate(c, chunk_size(i));
    size_.store(other.size_.exchange(0, std::memory_order_relaxed),
                std::memory_order_release);
  }

  T &operator[](size_t index) noexcept
  {
    x23assert(index < size());
    return get_index(index);
  }
  const T &operator[](size_t index) const noexcept
  {
    x23assert(index < size());
    return get_index(index);
  }

  size_t size() const noexcept
  {
    for (;;) {
      size_t size = size_.load(std::memory_order_acquire);
    recheck:
      if (size == kLocked) {
        if (!size_.compare_exchange_weak(size, kLockedWantNotify,
                                         std::memory_order_relaxed,
                                         std::memory_order_relaxed))
          goto recheck;
      }
      else if (size != kLockedWantNotify)
        return size;
      size_.wait(kLockedWantNotify);
    }
  }

  size_t emplace_back(auto &&...args)
  {
    size_t size = take_size();
    Defer _unlock([this, &size] { put_size(size); });

    auto l = Location::from_index(size);
    T *c = get_or_allocate_chunk(l.chunk());
    std::construct_at(c + l.item(), std::forward<decltype(args)>(args)...);
    return size++;
  }

  void resize(size_t n)
  {
    size_t size = take_size();
    Defer _unlock([this, &size] { put_size(size); });
    if (size == n)
      return;

    Location cur = Location::from_index(size);
    Location target = Location::from_index(n);
    if (cur < target)
      for (;;) {
        size = cur.to_index();
        auto s = nextspan(cur, target);
        if (s.empty())
          break;
        std::ranges::uninitialized_value_construct(s);
      }
    else {
      for (;;) {
        auto s = prevspan(cur, target);
        if (s.empty())
          break;
        std::ranges::destroy(s | std::views::reverse);
        size = cur.to_index();
        if (cur.item() == 0) {
          auto cn = cur.chunk();
          if (T *p = chunks_[cn].exchange(nullptr, std::memory_order_relaxed))
            a.deallocate(p, chunk_size(cn));
        }
      }
    }
  }

  iterator_type begin()
  {
    return iterator_type{this, 0};
  }
  iterator_type end()
  {
    return iterator_type{this, size()};
  }

private:
  static constexpr size_t kLogMinChunk =
      std::max(4, std::bit_width(4096 / sizeof(T)) - 1);
  static constexpr size_t kMinChunk = 1UZ << kLogMinChunk;

  static constexpr size_t kNumChunks =
      std::min<size_t>(23, std::bit_width((1UZ << (8 * sizeof(size_t) - 1)) /
                                          (kMinChunk * sizeof(T))));

  static constexpr size_t kLogMaxChunk = kLogMinChunk + kNumChunks - 1;
  static constexpr size_t kMaxChunk = 1UZ << kLogMaxChunk;

  struct Location {
    // Leave room to store chunks [0,kNumChunks] where kNumChunks can
    // be used for a sentinel value.
    static constexpr unsigned kChunkShift = 64 - std::bit_width(kNumChunks);

    // chunk << kChunkShift | item
    uint64_t bits_;

    constexpr Location() noexcept = default;
    constexpr Location(size_t chunkno, size_t itemno) noexcept
      : bits_(chunkno << kChunkShift | itemno)
    {}
    static Location from_index(size_t index)
    {
      // Let k = index/kMinChunk.  Chunk n contains 2^n values of k.
      // Specifically:
      //
      // Chunk 0: k in [0, 1)
      // Chunk 1: k in [1, 3)
      // Chunk 2: k in [3, 7)
      // Chunk n: k in [2^n - 1, 2^n - 1 + 2^n)
      //             = [2^n - 1, 2^(n+1) - 1)
      //
      // Inverting the above, k resides in chunk bit_width(k+1)-1.
      size_t chunkno = std::bit_width((index / kMinChunk) + 1) - 1;
      return Location(chunkno, index - chunk_start(chunkno));
    }

    size_t chunk() const noexcept { return bits_ >> kChunkShift; }
    size_t item() const noexcept
    {
      return bits_ & ((uint64_t(1) << kChunkShift) - 1);
    }
    size_t to_index() const noexcept { return chunk_start(chunk()) + item(); }

    decltype(auto) operator+=(this is_mutable auto &&self, ptrdiff_t d) noexcept
    {
      self.bits_ = from_index(self.to_index() + d).bits_;
      return (self);
    }
    auto operator++(this is_mutable auto &&self, int) noexcept
    {
      auto ret = self;
      self += 1;
      return ret;
    }
    decltype(auto) operator++(this is_mutable auto &&self) noexcept
    {
      return self += 1;
    }

    decltype(auto) operator-=(this is_mutable auto &&self, ptrdiff_t d) noexcept
    {
      self.bits_ = from_index(self.to_index() - d).bits_;
      return (self);
    }
    auto operator--(this is_mutable auto &&self, int) noexcept
    {
      auto ret = self;
      self -= 1;
      return ret;
    }
    decltype(auto) operator--(this is_mutable auto &&self) noexcept
    {
      return self -= 1;
    }

    template<typename Self>
    Self operator+(this const Self &self, difference_type d) noexcept
    {
      auto ret = self;
      ret += d;
      return ret;
    }
    template<references<Location> Self>
    friend Self operator+(difference_type d, const Self &self) noexcept
    {
      return self + d;
    }
    template<typename Self>
    Self operator-(this const Self &self, difference_type d) noexcept
    {
      auto ret = self;
      ret -= d;
      return ret;
    }
    template<references<Location> Self>
    friend Self operator-(difference_type d, const Self &self) noexcept
    {
      return self - d;
    }

    friend difference_type
    operator-(const references<Location> auto &a,
              const references<Location> auto &b) noexcept
    {
      if constexpr (requires { a.check_compat(b); })
        a.check_compat(b);
      return a.to_index() - b.to_index();
    }

    auto operator<=>(this const references<Location> auto &a,
                     const references<Location> auto &b) noexcept
    {
      if constexpr (requires { a.check_compat(b); })
        a.check_compat(b);
      return a.bits_ <=> b.bits_;
    }

    Location loc() const { return *this; }
  };

  static constexpr size_t kLocked = size_t(-1);
  static constexpr size_t kLockedWantNotify = kLocked - 1;

  // number of valid items, or kLocked/kLockedWantNotify while resizing
  mutable std::atomic_size_t size_{0};

  // chunks_[i] is nullptr or an array of (kMinChunk << i) allocations
  std::array<std::atomic<T *>, kNumChunks> chunks_{};

  static inline std::allocator<T> a{};

  // The chunks start at kMinChunk and keep doubling.
  static constexpr size_t chunk_size(unsigned chunkno) noexcept
  {
    return (1UZ << chunkno) * kMinChunk;
  }
  // The index of the first item in the chunk.  Since the chunks
  // start at size kMinChunk and keep doubling, the size of chunk k
  // is 2^k * kMinChunk, the sum of sizes of all previous chunks
  // will be (2^k - 1) * kMinChunk.
  static constexpr size_t chunk_start(unsigned chunkno) noexcept
  {
    return chunk_size(chunkno) - kMinChunk;
  }

  size_t take_size() noexcept
  {
    for (;;) {
      auto ret = size_.exchange(kLocked, std::memory_order_acquire);
      if (ret == kLocked)
        ret = size_.exchange(kLockedWantNotify, std::memory_order_acquire);
      if (ret != kLocked && ret != kLockedWantNotify)
        return ret;
      size_.wait(kLockedWantNotify);
    }
  }

  void put_size(size_t newsize) noexcept
  {
    auto sz = size_.exchange(newsize, std::memory_order_release);
    if (sz == kLockedWantNotify)
      size_.notify_all();
    else
      x23assert(sz == kLocked);
  }

  // Returns the largest contiguous span [start, end) where end <=
  // stop.  Advances start so that on the next call it returns the
  // next span.  Returns an empty span when start >= stop.
  std::span<T> nextspan(Location &start, Location stop)
  {
    if (start >= stop)
      return {};
    auto cn = start.chunk();
    T *c = get_or_allocate_chunk(cn);
    T *b = c + start.item();
    if (cn == stop.chunk()) {
      start = stop;
      return {b, c + stop.item()};
    }
    start = Location{cn + 1, 0};
    return {b, c + chunk_size(cn)};
  }

  // Returns the largest contiguous span [begin, start) where begin >=
  // stop.  Decreases start so that on the next call it returns the
  // next span.  Returns an empty span when start <= stop.
  std::span<T> prevspan(Location &start, Location stop)
  {
    for (;;) {
      if (start <= stop)
        return {};

      size_t cn = start.chunk();
      size_t in = start.item();
      if (in == 0) {
        --cn;
        in = chunk_size(cn);
      }

      if (T *c = chunks_[cn].load()) {
        if (cn == stop.chunk()) {
          start = stop;
          return {c + stop.item(), c + in};
        }
        start = Location{cn, 0};
        return {c, c + in};
      }
      start = Location{cn, 0};
    }
  }

  T *get_or_allocate_chunk(unsigned chunkno)
  {
    T *c = chunks_[chunkno].load();
    if (!c) {
      T *newc = a.allocate(chunk_size(chunkno));
      if (chunks_[chunkno].compare_exchange_strong(c, newc))
        c = newc;
      else
        a.deallocate(newc, chunk_size(chunkno));
    }
    return c;
  }

  T &get_location(Location l) const noexcept
  {
    return chunks_[l.chunk()].load(std::memory_order_relaxed)[l.item()];
  }
  T &get_index(size_t i) const noexcept
  {
    return get_location(Location::from_index(i));
  }
};

static_assert(std::random_access_iterator<NMVec<int>::iterator_type>);
static_assert(std::ranges::random_access_range<NMVec<int>>);

} // namespace x23
