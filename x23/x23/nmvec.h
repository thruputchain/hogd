// -*-C++-*-

#pragma once

#include <algorithm> // for ranges algorithms
#include <atomic>
#include <cstddef>
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

  void free(void *ptr, std::size_t n) noexcept
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

template<typename T> requires std::is_trivially_copyable_v<T>
class AppendOnlyVec {
public:
  using value_type = T;
  using pointer_type = value_type *;
  using atomic_pointer_type = std::atomic<pointer_type *>;

#ifdef __cpp_lib_is_layout_compatible
  static_assert(std::is_layout_compatible_v<pointer_type, atomic_pointer_type>);
#endif // __cpp_lib_is_layout_compatible

  value_type operator[](size_t i) const noexcept
  {
    return readat(i);
  }

private:
  static inline constexpr std::allocator<value_type> a{};
  std::mutex resize_lock_;
  detail::VecBuf<atomic_pointer_type> b_{};

  value_type readat(size_t i) const noexcept
  {
    value_type ret, *p;
    do {
      p = b_.begin.load(std::memory_order_relaxed);
      memcpy(&ret, p + i, sizeof(ret));
    } while (p == b_.begin.load());
    return ret;
  }

};

} // namespace x23
