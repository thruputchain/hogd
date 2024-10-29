#pragma once

#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <mutex>
#include <set>
#include <type_traits>
#include <vector>

namespace hogd {

namespace detail {

template<typename T> struct AllocationLine {
  T *ptr;
  const uint32_t line_highbits;
  uint32_t alloc_offset = 0;

  constexpr static uint32_t kAllocationLineBits = 17;
  constexpr static uint32_t kAllocationLineSize = static_cast<uint32_t>(1)
                                                  << kAllocationLineBits;

  void reset() { alloc_offset = 0; }

  uint32_t alloc()
  {
    uint32_t offset = alloc_offset;
    alloc_offset++;

    assert(alloc_offset <= kAllocationLineSize);

    return line_highbits + offset;
  }

  bool full() const { return alloc_offset == kAllocationLineSize; }
};

template<typename T> struct ThreadlocalRoundAllocator {
  std::vector<AllocationLine<T>> lines_;

  uint64_t freed_round_number_ = 0;

  // idempotent with same argument
  void release_deferred_frees(uint64_t round)
  {
    if (round > freed_round_number_) {
      for (auto &line : lines_) {
        line.reset();
      }
    }
  }

  void add_new_line(AllocationLine<T> line) { lines_.push_back(line); }

  uint32_t allocate()
  {
    for (auto &line : lines_) {
      if (!line.full()) {
        return line.alloc();
      }
    }
    return 0;
  }
};

} // namespace detail

// 0 is nullptr (anything less than kLineIndexMask is nullptr)

template<typename T> class ShortRoundAllocator {
public:
  constexpr static uint32_t kAllocationLineSize =
      detail::AllocationLine<T>::kAllocationLineSize;
  constexpr static uint32_t kAllocationLineBits =
      detail::AllocationLine<T>::kAllocationLineBits;

  constexpr static uint32_t kLineIndexMask =
      (static_cast<uint32_t>(1) << kAllocationLineBits) - 1;
  constexpr static uint32_t kLineIndexHighbitsIncrement =
      static_cast<uint32_t>(1) << kAllocationLineBits;

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

      uint32_t next_line =
          next_line_highbits_.fetch_add(kLineIndexHighbitsIncrement,
                                        std::memory_order_relaxed);

      auto *new_line = reinterpret_cast<T *>(make_new_line());
      lines_[(next_line >> kAllocationLineBits) - 1].store(
          new_line, std::memory_order_release);
      free_list_.add_new_line(
          detail::AllocationLine{.ptr = new_line, .line_highbits = next_line});

      res = free_list_.allocate();
    }

    T *arg = get(res);

    assert(!!arg);

    std::construct_at(arg, std::forward<Args>(args)...);

    return {res, arg};
  }

  void deferred_free(uint32_t ptr)
  {
    if constexpr (!std::is_trivially_destructible_v<T>) {
      auto *full_ptr = get(ptr);
      std::destroy_at(full_ptr);
    }
  }

  void release_deferred_frees(uint64_t round)
  {
    free_list_.release_deferred_frees(round);
  }

private:
  void *make_new_line() { return calloc(kAllocationLineSize, sizeof(T)); }

  void *get_line(uint32_t get_ptr)
  {
    return lines_[(get_ptr >> kAllocationLineBits) - 1].load(
        std::memory_order_relaxed);
  }

  std::atomic<uint32_t> next_line_highbits_ = kLineIndexHighbitsIncrement;
  std::array<std::atomic<void *>, 32768> lines_; // 2^15

  inline static thread_local detail::ThreadlocalRoundAllocator<T> free_list_;
};

} // namespace hogd
