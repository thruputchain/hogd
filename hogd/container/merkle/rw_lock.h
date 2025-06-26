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

  // TODO: Do not use x86 instructions.
  bool try_lock_reader()
  {
    while (true) {
      if (writer_waiting_for_upgrade_.load(std::memory_order_relaxed)) {
        return false;
      }

      int64_t cur;
      if ((cur = lock_.load(std::memory_order_relaxed)) == kWriterLocked) {
        return false;
      }
      // TODO(jhumphri): Is this safe? What is the writer gets the lock while
      // the reader is on this line, and then the writer deletes the node before
      // the reader runs the cmpxchg below?
      if (lock_.compare_exchange_weak(/*expected=*/cur, /*desired=*/cur + 1,
                                      /*success=*/std::memory_order_acquire,
                                      /*failure=*/std::memory_order_relaxed)) {
        return true;
      }
      // If the cmpxchg above fails, we try again because we expect there to be
      // a lot of readers contending on the lock.
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
    bool ret =
        lock_.compare_exchange_strong(expected, /*desired=*/kWriterLocked,
                                      /*success=*/std::memory_order_acquire,
                                      /*failure=*/std::memory_order_relaxed);
    return ret;
  }

  bool try_upgrade_to_writer_if_only_one()
  {
    {
      bool expected = false;
      if (!writer_waiting_for_upgrade_.compare_exchange_strong(
              expected, /*desired=*/true,
              /*success=*/std::memory_order_relaxed,
              /*failure=*/std::memory_order_relaxed)) {
        return false;
      }
    }

    {
      // Wait until we are the only reader left. This also ensures that we do
      // not race with another thread that calls [lock_writer()].
      int64_t expected = 1;
      while (
          !lock_.compare_exchange_weak(expected,
                                       /*desired=*/kWriterLocked,
                                       /*success=*/std::memory_order_acquire,
                                       /*failure=*/std::memory_order_relaxed)) {
        asm volatile("pause");
        // [expected] is overwritten with the value that was in [lock_] when the
        // cmpxchg was performed. We need to set [expected] back to [kUnlocked].
        expected = 1;
      }
    }

    // TODO(jhumphri): I could see this being an issue too.
    writer_waiting_for_upgrade_.store(false, std::memory_order_relaxed);
    return true;
  }

  bool is_locked_relaxed() const
  {
    return lock_.load(std::memory_order_relaxed) != kUnlocked;
  }

  bool is_writer_locked_relaxed() const
  {
    return lock_.load(std::memory_order_relaxed) == kWriterLocked;
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
  std::atomic<bool> writer_waiting_for_upgrade_ = false;
};

} // namespace hogd
