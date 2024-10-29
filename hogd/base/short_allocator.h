#pragma once

#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <mutex>
#include <set>
#include <vector>

#include "hogd/base/compiler.h"

namespace hogd {

namespace detail {

struct DeferrableFreeList {
  std::vector<uint32_t> free_indices_;
  std::vector<uint32_t> deferred_free_indices_;

  uint64_t freed_round_number_ = 0;

  void deferred_free(uint32_t ptr) { deferred_free_indices_.push_back(ptr); }

  // idempotent with same argument
  void release_deferred_frees(uint64_t round)
  {
    if (round > freed_round_number_) {
      freed_round_number_ = round;
      free_indices_.insert(free_indices_.end(), deferred_free_indices_.begin(),
                           deferred_free_indices_.end());
      deferred_free_indices_.clear();
    }
  }

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

} // namespace detail

// 0 is nullptr (anything less than kLineIndexMask is nullptr)

template<typename T> class ShortRoundAllocator {
public:
  constexpr static uint32_t kAllocationLineSize = static_cast<uint32_t>(1)
                                                  << 17;
  // 2^(ceil(log2(kAllocationLineSize))) - 1
  constexpr static uint32_t kLineIndexMask = kAllocationLineSize - 1;

  T *get(uint32_t ptr)
  {
    if (ptr <= kLineIndexMask) {
      return nullptr;
    }

    T *line = reinterpret_cast<T *>(get_line(ptr));
    return &(line[ptr & kLineIndexMask]);
  }

  template<typename... Args> std::pair<uint32_t, T *> allocate_new(Args... args)
  {
    auto res = free_list_.allocate();
    if (res == 0) {
      res = next_.fetch_add(1, std::memory_order_relaxed);
    }

    T *arg = get(res);

    assert(!!arg);

    std::construct_at(arg, std::forward<Args>(args)...);

    return {res, arg};
  }

  void deferred_free(uint32_t ptr) { free_list_.deferred_free(ptr); }

  void release_deferred_frees(uint64_t round)
  {
    free_list_.release_deferred_frees(round);
  }

private:
  void *make_new_line() { return calloc(kAllocationLineSize, sizeof(T)); }

  void *get_line(uint32_t get_ptr)
  {
    auto &line_ptr = lines_[(get_ptr >> 17) - 1];
    void *new_ptr = nullptr;

    auto *ptr = line_ptr.load(std::memory_order_relaxed);
    while (true) {
      if (ptr) {
        free(new_ptr);
        return ptr;
      }
      if (new_ptr == nullptr) {
        new_ptr = make_new_line();
      }
      if (line_ptr.compare_exchange_strong(ptr, new_ptr,
                                           std::memory_order_release)) {
        return new_ptr;
      }
      cpu_relax();
    }
  }

  std::atomic<uint32_t> next_ = kLineIndexMask + 1;
  std::array<std::atomic<void *>, 32768> lines_; // 2^15

  inline static thread_local detail::DeferrableFreeList free_list_;
};

} // namespace hogd
