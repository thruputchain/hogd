#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "x23/testlock.h"

using namespace x23;

TEST(TestLock, TryLockBusyAndCrossThreadUnlock)
{
  TestLock lock;
  EXPECT_FALSE(lock.busy());
  lock.block_until_free();
  ASSERT_TRUE(lock.try_lock());
  EXPECT_TRUE(lock.busy());
  EXPECT_FALSE(lock.try_lock());
  EXPECT_FALSE(lock.try_lock(true));
  std::jthread unlocker([&] { lock.unlock(); });
  lock.block_until_free();
  EXPECT_FALSE(lock.busy());
  EXPECT_TRUE(lock.try_lock());
  lock.unlock();
}

TEST(TestLock, ContendingLockersPreserveProtectedWrites)
{
  TestLock lock;
  unsigned count = 0;
  std::vector<std::jthread> threads;
  for (unsigned i = 0; i < 8; ++i)
    threads.emplace_back([&] {
      for (unsigned n = 0; n < 10000; ++n) {
        std::lock_guard guard(lock);
        ++count;
      }
    });
  threads.clear();
  EXPECT_EQ(count, 80000U);
  EXPECT_FALSE(lock.busy());
}

TEST(SharedLock, ExclusiveWaitsForMovedSharedHolders)
{
  SharedLock lock(*core_map);
  EXPECT_EQ(&lock.map(), &*core_map);
  auto first = lock.hold_shared();
  auto second = lock.hold_shared();
  auto moved = std::move(first);
  first.reset();
  std::atomic<bool> entered{false};
  std::jthread writer([&] {
    std::lock_guard guard(lock);
    entered = true;
  });
  // busy() becomes true before the writer waits for shared holders.
  while (!lock.busy())
    std::this_thread::yield();
  EXPECT_FALSE(entered.load());
  second.reset();
  EXPECT_FALSE(entered.load());
  std::jthread releaser(
      [holder = std::move(moved)]() mutable { holder.reset(); });
  releaser.join();
  writer.join();
  EXPECT_TRUE(entered.load());
  EXPECT_FALSE(lock.busy());
}

TEST(SharedLock, SharedAcquisitionWaitsForExclusiveUnlock)
{
  SharedLock lock;
  lock.lock();
  std::atomic<bool> started{false}, entered{false};
  int value = 0;
  std::jthread reader([&] {
    started = true;
    auto held = lock.hold_shared();
    EXPECT_EQ(value, 42);
    entered = true;
  });
  while (!started.load())
    std::this_thread::yield();
  EXPECT_FALSE(entered.load());
  value = 42;
  lock.unlock();
  reader.join();
  EXPECT_TRUE(entered.load());
}
