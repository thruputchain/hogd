#pragma once

#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <mutex>
#include <set>
#include <vector>

#include "hogd/base/compiler.h"
#include "hogd/base/non_movable.h"
#include "hogd/base/threadlocal_cache.h"

namespace hog {

namespace detail {
// Only supports single-threaded operation
struct SequentialFreeList {
  // FIFO, to avoid excessive move
  std::vector<uint32_t> free_indices_;

  void free(uint32_t ptr) { free_indices_.push_back(ptr); }

  uint32_t allocate()
  {
    if (free_indices_.empty()) {
      return 0;
    }

    uint32_t back = free_indices_.back();
    free_indices_.pop_back();
    return back;
  }
};

struct AllocationVerifier {

  std::set<uint32_t> allocs_;
  std::mutex mtx_;

  void check_get(uint32_t ptr);
  void check_allocate(uint32_t ptr);
  void check_free(uint32_t ptr);

  void assert_empty() const noexcept;

  ~AllocationVerifier();
};

struct EmptyVerifier {
  void check_get(uint32_t ptr) {}
  void check_allocate(uint32_t ptr) {}
  void check_free(uint32_t ptr) {}
};

} // namespace detail

// 0 is implicit nullptr

// THIS PERFORMS NO CONSTRUCTORS OR DESTRUCTORS

template<typename T> class ShortAllocator : NonMovableOrCopyable {
public:
  constexpr static uint32_t kAllocationLineSize = static_cast<uint32_t>(1)
                                                  << 17;
  constexpr static uint32_t kLineIndexMask = kAllocationLineSize - 1;

  constexpr static bool kIsDebugBuild = hogd::is_debug_build();

  T *get(uint32_t ptr)
  {
    if constexpr (kIsDebugBuild) {
      verifier_.check_get(ptr);
    }

    if (ptr <= kLineIndexMask) {
      return nullptr;
    }
    T *line = reinterpret_cast<T *>(get_line(ptr));
    return &(line[ptr & kLineIndexMask]);
  }

  uint32_t allocate()
  {
    auto res = free_list_.get().allocate();
    if (res == 0) {
      res = next_.fetch_add(1, std::memory_order_relaxed);
    }

    if constexpr (kIsDebugBuild) {
      verifier_.check_allocate(res);
    }

    return res;
  }

  void free(uint32_t ptr)
  {
    if constexpr (kIsDebugBuild) {
      verifier_.check_free(ptr);
    }
    free_list_.get().free(ptr);
  }

private:
  uint8_t *make_new_line()
  {
    return new uint8_t[sizeof(T) * kAllocationLineSize];
  }

  uint8_t *get_line(uint32_t get_ptr)
  {
    auto &line_ptr = lines_[(get_ptr >> 17) - 1];
    uint8_t *new_ptr = nullptr;

    auto *ptr = line_ptr.load(std::memory_order_relaxed);
    while (true) {
      if (ptr) {
        delete[] new_ptr;
        return ptr;
      }
      if (new_ptr == nullptr) {
        new_ptr = make_new_line();
      }
      if (line_ptr.compare_exchange_strong(ptr, new_ptr,
                                           std::memory_order_release)) {
        return new_ptr;
      }

      // SPINLOCK_PAUSE();
    }
  }

  std::atomic<uint32_t> next_ = kLineIndexMask + 1;
  ThreadlocalCache<detail::SequentialFreeList, 256> free_list_;

  std::array<std::atomic<uint8_t *>, 32768> lines_; // 2^15

  std::conditional_t<kIsDebugBuild, detail::AllocationVerifier,
                     detail::EmptyVerifier>
      verifier_;
};

} // namespace hog
