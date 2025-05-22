#pragma once

#include <atomic>
#include <cassert>

#include "hogd/base/types.h"

namespace hogd {

class InvalidStateFlag {
public:
  InvalidStateFlag() = default;

  InvalidStateFlag(InvalidStateFlag const &) = delete;
  InvalidStateFlag &operator=(InvalidStateFlag const &) = delete;
  InvalidStateFlag(InvalidStateFlag &&) = delete;
  InvalidStateFlag &operator=(InvalidStateFlag &&) = delete;

  void mark_as_invalid()
  {
    invalid_state_count_.fetch_add(1, std::memory_order_relaxed);
  }

  void unmark_as_invalid()
  {
    [[maybe_unused]] auto old_value =
        invalid_state_count_.fetch_sub(1, std::memory_order_relaxed);
    assert(old_value > 0);
  }

  bool is_invalid() const
  {
    return invalid_state_count_.load(std::memory_order_relaxed) > 0;
  }

private:
  std::atomic<int64_t> invalid_state_count_ = 0;
};

} // namespace hogd
