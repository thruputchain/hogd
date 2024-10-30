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
  T *ptr_;
  const uint32_t line_highbits_;
  uint32_t allocation_offset_ = 0;
  uint32_t max_allocated_ = 0;

  constexpr static uint32_t kAllocationLineBits = 17;
  constexpr static uint32_t kAllocationLineSize = uint32_t{1}
                                                  << kAllocationLineBits;

  void reset() { allocation_offset_ = 0; }

  uint32_t allocate()
  {
    uint32_t offset = allocation_offset_;
    allocation_offset_++;

    assert(allocation_offset_ <= kAllocationLineSize);

    if (offset < max_allocated_) {
      if constexpr (!std::is_trivially_destructible_v<T>) {
        std::destroy_at(ptr_ + offset);
      }
    }
    max_allocated_ = std::max(max_allocated_, allocation_offset_);

    return line_highbits_ + offset;
  }

  bool full() const { return allocation_offset_ == kAllocationLineSize; }
};

// This object is held for exclusive use by one thread.
// The AllocationLines are created by the RoundAllocator
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
      freed_round_number_ = round;
    }
  }

  void add_new_line(AllocationLine<T> line) { lines_.push_back(line); }

  uint32_t allocate()
  {
    for (auto &line : lines_) {
      if (!line.full()) {
        return line.allocate();
      }
    }
    return 0;
  }
};

} // namespace detail

// There is one RoundAllocator, which contains for each thread a
// ThreadlocalRoundAllocator. Each ThreadlocalRoundAllocator is modified only by
// the corresponding thread, but elements within the allocator can be modified
// by any thread. Each ThreadlocalRoundAllocator receives new AllocationLines
// when their lines are exhausted.
//
// 0 is nullptr (anything less than kLineIndexMask is nullptr)
template<typename T, bool VerifyAllocations = false> class RoundAllocator {
public:
  constexpr static uint32_t kAllocationLineSize =
      detail::AllocationLine<T>::kAllocationLineSize;
  constexpr static uint32_t kAllocationLineBits =
      detail::AllocationLine<T>::kAllocationLineBits;

  constexpr static uint32_t kLineIndexMask =
      (uint32_t{1} << kAllocationLineBits) - 1;
  constexpr static uint32_t kLineIndexHighbitsIncrement =
      uint32_t{1} << kAllocationLineBits;

  T *get(uint32_t ptr)
  {
    if (ptr <= kLineIndexMask) {
      return nullptr;
    }

    if constexpr (VerifyAllocations) {
      verifier_.check_get(ptr);
    }

    auto *line = get_line(ptr);
    return &(line[ptr & kLineIndexMask]);
  }

  // Returns both the short ptr and an actual pointer to the newly-constructed
  // object. (the short pointer cannot be easily recovered from the actual
  // pointer)
  template<typename... Args> std::pair<uint32_t, T *> construct(Args &&...args)
  {
    auto res = threadlocal_allocator_.allocate();
    if (res == 0) {

      uint32_t next_line =
          next_line_highbits_.fetch_add(kLineIndexHighbitsIncrement,
                                        std::memory_order_relaxed);

      auto *new_line = allocate_line();
      lines_[(next_line >> kAllocationLineBits) - 1].store(
          new_line, std::memory_order_release);
      threadlocal_allocator_.add_new_line(
          detail::AllocationLine{.ptr_ = new_line,
                                 .line_highbits_ = next_line});

      res = threadlocal_allocator_.allocate();
      assert(res != 0);
    }

    if constexpr (VerifyAllocations) {
      verifier_.alloc(res);
    }

    T *arg = get(res);

    assert(arg);

    std::construct_at(arg, std::forward<Args>(args)...);

    return {res, arg};
  }

  // Release deferred-free objects for the current thread.
  void release_deferred_frees(uint64_t round)
  {
    threadlocal_allocator_.release_deferred_frees(round);
  }

  // no-op, but the calls are left in for debugging purposes
  void deferred_free(uint32_t ptr)
  {
    if constexpr (VerifyAllocations) {
      if (ptr) {
        verifier_.free(ptr);
      }
    }
  }

private:
  T *allocate_line()
  {
    return static_cast<T *>(malloc(kAllocationLineSize * sizeof(T)));
  }

  T *get_line(uint32_t get_ptr)
  {
    return lines_[(get_ptr >> kAllocationLineBits) - 1].load(
        std::memory_order_relaxed);
  }

  std::atomic<uint32_t> next_line_highbits_ = kLineIndexHighbitsIncrement;
  std::array<std::atomic<T *>, 32768> lines_; // 2^15

  inline static thread_local detail::ThreadlocalRoundAllocator<T>
      threadlocal_allocator_;

  using verifier_t =
      std::conditional_t<VerifyAllocations, detail::AllocationVerifier,
                         detail::EmptyVerifier>;

  verifier_t verifier_;
};

} // namespace hogd
