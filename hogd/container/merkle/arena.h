// arena.h - provides an arena for allocating memory.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>

#include <format>
#include <iomanip>
#include <ios>
#include <iterator>
#include <ostream>
#include <sstream>
#include <stack>
#include <string>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "types.hh"

namespace hogd {

// An arena. This has a very simple implementation right now and needs more
// work. For example, we currently do not free any memory that we allocate.
//
// Example:
// Arena arena(/*alloc_size=*/64);
//
// void *ptr = arena.alloc();
// CHECK_PTR(ptr);
//
// arena.free(ptr);
class Arena {
public:
  // Creates an arena that allocates memory for objects of size [alloc_size].
  Arena(size_t alloc_size)
    : alloc_size_(alloc_size), batch_alloc_size_(alloc_size * kBatchSize)
  {
    CHECK_GT(alloc_size_, 0);
  }

  // Disable copy and move.
  Arena(const Arena &) = delete;
  Arena &operator=(const Arena &) = delete;
  Arena(Arena &&) noexcept = delete;
  Arena &operator=(Arena &&) noexcept = delete;

  // Allocates a new memory region.
  void *alloc()
  {
    if (ptrs_.empty()) {
      // When we need to malloc more memory, we malloc enough memory to hold a
      // batch of objects rather than just a single object. This amortizes the
      // malloc overhead.
      char *batch =
          static_cast<char *>(aligned_alloc(kCacheLineSize, batch_alloc_size_));
      for (size_t i = 0; i < batch_alloc_size_; i += alloc_size_) {
        ptrs_.push(static_cast<void *>(batch + i));
      }
    }
    void *ret = ptrs_.top();
    ptrs_.pop();
    return ret;
  }

  // Frees a memory region. [ptr] must have been returne by [alloc()]
  // previously. The behavior is undefined if not.
  void free(void *ptr)
  {
    CHECK_PTR(ptr);

    ptrs_.push(ptr);
  }

private:
  static constexpr size_t kCacheLineSize = 64;
  // The number of objects we allocate when we call malloc. By allocating a
  // batch of objects, we amortize the malloc overhead.
  static constexpr size_t kBatchSize = 64;
  // The size of the objects the arena will allocate.
  const size_t alloc_size_;
  // The size of the batch the arena allocates with malloc.
  const size_t batch_alloc_size_;

  // TODO(jhumphri): Use Adam's thread_local implementation.
  // Tracks the objects allocated by the arena.
  inline static thread_local std::stack<void *> ptrs_;
};

} // namespace hogd
