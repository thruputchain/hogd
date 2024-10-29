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

// Allocator for short-lived objects using 32-bit pointers.
// Each thread reserves a threadlocal set of allocation buffers,
// allocating from its active buffer by incrementing a counter.
// Threads receive additional buffers when they exhaust what they have.
// Threads DO NOT automatically return buffers to the main allocation when they
// terminate. As such, these methods should only be accessed by, e.g., a pool of
// worker threads.
//
// Each 32-bit pointer is decomposable into a 15-bit buffer index,
// and a 17-bit offset into a buffer (these choices are arbitrary).
//
// This design relies on a monotonically-increasing ``round counter''.
// All allocated objects are freed when this round counter increases.
//
// TODO(geoff): Investigate whether deferring destructors until the end
// causes a latency problem.  They could be deferred lazily until reallocation.
namespace detail {

struct AllocationVerifier {
  std::set<uint32_t> allocated_ptrs;
  std::mutex mtx;

  void free(uint32_t ptr)
  {
    std::lock_guard lock(mtx);
    if (!allocated_ptrs.contains(ptr)) {
      throw std::runtime_error("double free on short ptr");
    }
    allocated_ptrs.erase(ptr);
  }

  void alloc(uint32_t ptr)
  {
    std::lock_guard lock(mtx);

    if (allocated_ptrs.contains(ptr)) {
      throw std::runtime_error("double allocation on short ptr");
    }

    allocated_ptrs.insert(ptr);
  }

  void check_get(uint32_t ptr)
  {
    std::lock_guard lock(mtx);

    if (!allocated_ptrs.contains(ptr)) {
      throw std::runtime_error("invalid access on short ptr");
    }
  }
};

struct EmptyVerifier {};

template<typename T> struct AllocationLine {
  T *ptr;
  const uint32_t line_highbits;
  uint32_t alloc_offset = 0;

  constexpr static uint32_t kAllocationLineBits = 17;
  constexpr static uint32_t kAllocationLineSize = static_cast<uint32_t>(1)
                                                  << kAllocationLineBits;

  void reset()
  {
    if constexpr (!std::is_trivially_destructible_v<T>) {
      for (uint32_t idx = 0; idx < alloc_offset; idx++) {
        std::destroy_at(ptr + idx);
      }
    }
    alloc_offset = 0;
  }

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

template<typename T, bool VerifyAllocations = false>
class ShortRoundAllocator {
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

    if constexpr (VerifyAllocations) {
      verifier_.check_get(ptr);
    }

    T *line = reinterpret_cast<T *>(get_line(ptr));
    return &(line[ptr & kLineIndexMask]);
  }

  template<typename... Args> std::pair<uint32_t, T *> allocate_new(Args... args)
  {
    auto res = threadlocal_allocator_.allocate();
    if (res == 0) {

      uint32_t next_line =
          next_line_highbits_.fetch_add(kLineIndexHighbitsIncrement,
                                        std::memory_order_relaxed);

      auto *new_line = reinterpret_cast<T *>(make_new_line());
      lines_[(next_line >> kAllocationLineBits) - 1].store(
          new_line, std::memory_order_release);
      threadlocal_allocator_.add_new_line(
          detail::AllocationLine{.ptr = new_line, .line_highbits = next_line});

      res = threadlocal_allocator_.allocate();
      assert(res != 0);
    }

    if constexpr (VerifyAllocations) {
      verifier_.alloc(res);
    }

    T *arg = get(res);

    assert(!!arg);

    std::construct_at(arg, std::forward<Args>(args)...);

    return {res, arg};
  }

  void release_deferred_frees(uint64_t round)
  {
    threadlocal_allocator_.release_deferred_frees(round);
  }

  void deferred_free(uint32_t ptr)
  {
    if constexpr (VerifyAllocations) {
      if (ptr) {
        verifier_.free(ptr);
      }
    }
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

  inline static thread_local detail::ThreadlocalRoundAllocator<T>
      threadlocal_allocator_;

  using verifier_t =
      std::conditional_t<VerifyAllocations, detail::AllocationVerifier,
                         detail::EmptyVerifier>;

  verifier_t verifier_;
};

} // namespace hogd
