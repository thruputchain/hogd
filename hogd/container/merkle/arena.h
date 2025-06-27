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

class Arena {
public:
  Arena(size_t alloc_size)
    : alloc_size_(alloc_size), batch_alloc_size_(alloc_size * kBatchSize)
  {
    CHECK_GT(alloc_size_, 0);
  }

  void *alloc()
  {
    if (ptrs_.empty()) {
      // Allocate another page.
      char *page = static_cast<char *>(malloc(batch_alloc_size_));
      for (size_t i = 0; i < batch_alloc_size_; i += alloc_size_) {
        ptrs_.push(static_cast<void *>(page + i));
      }
    }
    void *ret = ptrs_.top();
    ptrs_.pop();
    return ret;
  }

  void free(void *ptr)
  {
    CHECK_PTR(ptr);

    ptrs_.push(ptr);
  }

  // Disable copy and move.
  Arena(const Arena &) = delete;
  Arena &operator=(const Arena &) = delete;
  Arena(Arena &&) noexcept = delete;
  Arena &operator=(Arena &&) noexcept = delete;

private:
  static constexpr size_t kBatchSize = 64;
  const size_t alloc_size_;
  const size_t batch_alloc_size_;

  // TODO(jhumphri): Use Adam's thread_local implementation.
  static thread_local std::stack<void *> ptrs_;
};

} // namespace hogd
