// rw_lock.h - provides a read/write lock with the ability to upgrade a reader
// to a writer without dropping the lock.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>

#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"

namespace hogd {

// A read/write lock. Either multiple readers or a single writer can hold the
// lock at any time.
//
// Example:
// RwLock lock;
//
// while (!lock.try_lock_reader()) { _mm_pause(); }
// --The thread now holds the lock in reader mode.--
// lock.unlock();
//
// lock.lock_writer();
// --The thead now holds the lock in writer mode.--
// lock.unlock();
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
      int64_t cur = lock_.load(std::memory_order_relaxed);
      if (cur == kWriterLocked) {
        // The lock is currently held by a writer.
        cpu_relax();
        continue;
      }
      if (!lock_.compare_exchange_weak(/*expected=*/cur, /*desired=*/cur + 1,
                                       /*success=*/std::memory_order_acquire,
                                       /*failure=*/std::memory_order_relaxed)) {
        cpu_relax();
        continue;
      }
      break;
    }
  }

  // Tries to acquire the lock in reader mode. Returns false if a writer holds
  // the lock or if a writer is trying to acquire the lock. Otherwise, spins
  // until the lock is acquired in reader mode and returns true.
  bool try_lock_reader()
  {
    while (true) {
      if (writer_waiting_for_upgrade_.load(std::memory_order_relaxed)) {
        // A writer is trying to acquire the lock, so return false. This ensures
        // that a writer is not starved when there is a large number of readers.
        return false;
      }

      int64_t cur = lock_.load(std::memory_order_relaxed);
      if (cur == kWriterLocked) {
        // The lock is currently held by a writer.
        return false;
      }
      if (lock_.compare_exchange_weak(/*expected=*/cur, /*desired=*/cur + 1,
                                      /*success=*/std::memory_order_acquire,
                                      /*failure=*/std::memory_order_relaxed)) {
        return true;
      }
      cpu_relax();
    }
  }

  // Acquires the lock in writer mode.
  void lock_writer()
  {
    int64_t expected = kUnlocked;
    while (
        !lock_.compare_exchange_weak(expected,
                                     /*desired=*/kWriterLocked,
                                     /*success=*/std::memory_order_acquire,
                                     /*failure=*/std::memory_order_relaxed)) {
      cpu_relax();
      expected = kUnlocked;
    }
  }

  // Tries to acquire the lock in writer mode. Returns true if successful, false
  // otherwise.
  bool try_lock_writer()
  {
    int64_t expected = kUnlocked;
    bool ret =
        lock_.compare_exchange_strong(expected, /*desired=*/kWriterLocked,
                                      /*success=*/std::memory_order_acquire,
                                      /*failure=*/std::memory_order_relaxed);
    return ret;
  }

  // Tries to upgrade the lock from a reader lock to a writer lock. This
  // function assumes the current thread already holds the lock in reader mode.
  // The advantage of this function is that a thread does not need to drop the
  // lock altogether before trying to acquire it as a writer. During the period
  // where the thread does not hold the lock, another thread could acquire the
  // lock as a writer and make a dangerous change, such as deleting the large
  // structure that this lock protects.
  bool try_upgrade_to_writer_if_only_one()
  {
    CHECK(is_reader_locked_relaxed());

    {
      bool expected = false;
      if (!writer_waiting_for_upgrade_.compare_exchange_strong(
              expected, /*desired=*/true,
              /*success=*/std::memory_order_relaxed,
              /*failure=*/std::memory_order_relaxed)) {
        // Another thread is already trying to acquire the lock in writer mode,
        // so return false.
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
        cpu_relax();
        expected = 1;
      }
    }

    writer_waiting_for_upgrade_.store(false, std::memory_order_relaxed);
    return true;
  }

  bool is_locked_relaxed() const
  {
    return lock_.load(std::memory_order_relaxed) != kUnlocked;
  }

  bool is_reader_locked_relaxed() const
  {
    return lock_.load(std::memory_order_relaxed) > kUnlocked;
  }

  bool is_writer_locked_relaxed() const
  {
    return lock_.load(std::memory_order_relaxed) == kWriterLocked;
  }

  // Unlocks the lock, regardless of whether the current thread is a reader or a
  // writer.
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
  // A sentinel value that indicates the lock is unlocked.
  static constexpr int64_t kUnlocked = 0;
  // A sentinel value that indicates the lock is held by a writer.
  static constexpr int64_t kWriterLocked = -1;

  void unlock_reader() { lock_.fetch_sub(1, std::memory_order_release); }
  void unlock_writer() { lock_.store(kUnlocked, std::memory_order_release); }

  // The spinlock. Holds a value of [kUnlocked] (0) when unlocked. Holds a value
  // of [kWriterLocked] (-1) when locked by a writer. Holds a positive value
  // when locked by one or more readers; the exact positive value is the number
  // of readers currently holding the lock.
  std::atomic<int64_t> lock_ = kUnlocked;
  // This is true when a writer is trying to acquire the lock. While this value
  // is true, no additional readers will be allowed to acquire the lock. This
  // prevents a writer from being starved when there is a large number of
  // readers.
  std::atomic<bool> writer_waiting_for_upgrade_ = false;
};

} // namespace hogd
