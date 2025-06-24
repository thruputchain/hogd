// rw_lock.h - provides a read/write lock with the ability to upgrade a reader
// to a writer without dropping the lock.

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
#include <string>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "types.hh"

namespace hogd {

class RwLock {
public:
  RwLock() = default;

  // Disable copy and move.
  RwLock(const RwLock &) = delete;
  RwLock &operator=(const RwLock &) = delete;
  RwLock(RwLock &&) noexcept = delete;
  RwLock &operator=(RwLock &&) noexcept = delete;

  void lock_reader()
  {
    while (true) {
      int64_t cur;
      while ((cur = lock_.load(std::memory_order_relaxed)) == kWriterLocked) {
        asm volatile("pause");
      }
      if (lock_.compare_exchange_weak(/*expected=*/cur, /*desired=*/cur + 1,
                                      /*success=*/std::memory_order_acquire,
                                      /*failure=*/std::memory_order_relaxed)) {
        return;
      }
      asm volatile("pause");
      // [cur] is overwritten with the value that was in [lock_] when the
      // cmpxchg was performanced. [cur] is set again in the while loop
      // statement above.
    }
  }

  void lock_writer()
  {
    int64_t expected = kUnlocked;
    while (
        !lock_.compare_exchange_weak(expected,
                                     /*desired=*/kWriterLocked,
                                     /*success=*/std::memory_order_acquire,
                                     /*failure=*/std::memory_order_relaxed)) {
      asm volatile("pause");
      // [expected] is overwritten with the value that was in [lock_] when the
      // cmpxchg was performed. We need to set [expected] back to [kUnlocked].
      expected = kUnlocked;
    }
  }

  bool try_lock_writer()
  {
    int64_t expected = kUnlocked;
    return lock_.compare_exchange_strong(expected, /*desired=*/kWriterLocked,
                                         /*success=*/std::memory_order_acquire,
                                         /*failure=*/std::memory_order_relaxed);
  }

  void print_lock() const
  {
    printf("lock: %ld (%p)\n", lock_.load(std::memory_order_relaxed), this);
  }

  bool is_locked_relaxed() const
  {
    return lock_.load(std::memory_order_relaxed) != kUnlocked;
  }

  void unlock()
  {
    int64_t cur = lock_.load(std::memory_order_relaxed);
    CHECK_NE(cur, kUnlocked);
    if (cur == kWriterLocked) {
      unlock_writer();
    }
    else {
      unlock_reader();
    }
  }

private:
  static constexpr int64_t kUnlocked = 0;
  static constexpr int64_t kWriterLocked = -1;

  void unlock_reader() { lock_.fetch_sub(1, std::memory_order_release); }
  void unlock_writer() { lock_.store(kUnlocked, std::memory_order_release); }

  std::atomic<int64_t> lock_ = kUnlocked;
};

} // namespace hogd
