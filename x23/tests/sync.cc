
#include <atomic>
#include <mutex>
#include <print>
#include <thread>

#include <unistd.h>

#include <gtest/gtest.h>

#include "x23/event.h"
#include "x23/testlock.h"
#include "x23/threadpool.h"

using namespace x23;

ThreadPool pool;
CoroPoll cp(pool);

TEST(TestLock, MutualExclusion)
{
  std::atomic_bool ready{false};
  int n = 0;
  TestLock lock;
  std::jthread t([&] {
    std::lock_guard g(lock);
    ready = true;
    usleep(50'000);
    n = 1;
  });
  while (!ready)
    ;
  std::lock_guard g(lock);
  ASSERT_EQ(n, 1);
}

TEST(TestLock, ManyLockers)
{
  TestLock lock;
  std::thread::id tid{};
  std::array<std::jthread, 64> threads;
  auto go = [&] {
    std::thread::id myid = std::this_thread::get_id();
    std::lock_guard g(lock);
    EXPECT_NE(tid, myid);
    tid = myid;
    usleep(1'000);
    EXPECT_EQ(tid, myid);
  };
  for (auto &thread : threads)
    thread = std::jthread(go);
}

TEST(SharedLock, MutualExclusion)
{
  std::atomic_bool ready{false};
  int n = 0;
  SharedLock lock;
  std::jthread t([&] {
    std::lock_guard g(lock);
    ready = true;
    usleep(50'000);
    n = 1;
  });
  while (!ready)
    ;
  auto holder = lock.hold_shared();
  ASSERT_EQ(n, 1);
}

TEST(SharedLock, ManyReaders)
{
  std::atomic_uint nthreads = 0x1'0000;

  SharedLock lock;
  lock.lock();
  std::jthread writer([&] {
    usleep(50'000);
    nthreads = 0;
    lock.unlock();
    nthreads.notify_all();
  });

  std::array<std::jthread, 64> readers;
  auto go = [&] {
    auto holder = lock.hold_shared();
    ASSERT_LE(nthreads, readers.size());
    if (++nthreads == readers.size())
      nthreads.notify_all();
    unsigned int n;
    while ((n = nthreads) != readers.size())
      nthreads.wait(n);
  };
  for (auto &t : readers)
    t = std::jthread{go};
}

TEST(CoroLock, MutualExclusion)
{
  [] -> Async<void> {
    std::atomic_bool ready{false};
    int n = 0;
    CoroLock lock;
    [](std::atomic_bool &ready, int &n, CoroLock &lock) -> Detached {
      co_await pool.bg();
      CoroGuard g1;
      co_await g1(lock);
      ready = true;
      ready.notify_all();
      usleep(50'000);
      n = 1;
      g1.reset();
    }(ready, n, lock);
    while (!ready)
      ready.wait(false);
    CoroGuard g2;
    co_await g2(lock);
    EXPECT_EQ(n, 1);
    co_return;
  }()();
}

TEST(CoroLock, ManyLockers)
{
  [] -> Async<void> {
    CoroLock lock;
    CoroGuard g;
    co_await g(lock);
    std::atomic_int lastid = -1;

    for (int i = 0; i < 64; ++i)
      [](CoroLock &lock, std::atomic_int &lastid, int myid) -> Detached {
        co_await pool.bg();
        CoroGuard g;
        co_await g(lock);
        EXPECT_GE(lastid, 0);
        EXPECT_NE(lastid, myid);
        lastid = myid;
        usleep(2'000);
        EXPECT_EQ(lastid, myid);
      }(lock, lastid, i);

    usleep(50'000);
    lastid = 0x1'0000;
    g.reset();
    co_await pool.async(&ThreadPool::drain, &pool);
  }()();
}

TEST(CoroSharedLock, MutualExclusion)
{
  [] -> Async<void> {
    std::atomic_bool ready{false};
    int n = 0;
    CoroSharedLock<> lock(pool);
    [](std::atomic_bool &ready, int &n, CoroSharedLock<> &lock) -> Detached {
      co_await pool.bg();
      CoroGuard g1;
      co_await g1(lock);
      ready = true;
      ready.notify_all();
      usleep(50'000);
      n = 1;
      g1.reset();
    }(ready, n, lock);
    while (!ready)
      ready.wait(false);
    auto holder = co_await lock.coro_lock_shared();
    EXPECT_EQ(n, 1);
    holder.reset();
    co_return;
  }()();
}

TEST(CoroSharedLock, ManyReaders)
{
  [] -> Async<void> {
    std::atomic_bool ready = false;
    std::atomic_uint nthreads = 0x1'0000;
    CoroSharedLock lock(pool);
    [](auto &ready, auto &nthreads, CoroSharedLock<> &lock) -> Detached {
      co_await pool.bg();
      CoroGuard g;
      co_await g(lock);
      ready = true;
      usleep(50'000);
      nthreads = 0;
    }(ready, nthreads, lock);

    constexpr size_t nreaders = 64;
    while (!ready)
      co_await pool.bg();
    auto go = [&] -> Detached {
      co_await pool.bg();
      auto holder = co_await lock.coro_lock_shared();
      nthreads++;
    };
    for (auto i = 0UZ; i < nreaders; ++i)
      go();
    while (nthreads != nreaders)
      co_await pool.bg();
    co_await pool.async(&ThreadPool::drain, &pool);
  }()();
}

TEST(CoroSharedLock, TestLock)
{
  [] -> Async<void> {
    std::atomic_bool ready = false;
    std::atomic_uint nthreads = 0x1'0000;
    CoroSharedLock<TestLock> lock(pool);
    auto writer = [&] {
      std::lock_guard g(lock);
      ready = true;
      usleep(50'000);
      nthreads = 0;
    };
    std::jthread j(writer);

    constexpr size_t nreaders = 64;
    while (!ready)
      co_await pool.bg();
    auto go = [&] -> Detached {
      co_await pool.bg();
      auto holder = co_await lock.coro_lock_shared();
      nthreads++;
    };
    for (auto i = 0UZ; i < nreaders; ++i)
      go();
    while (nthreads != nreaders)
      co_await pool.bg();
    co_await pool.async(&ThreadPool::drain, &pool);
  }()();
}
