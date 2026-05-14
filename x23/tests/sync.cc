
#include <atomic>
#include <coroutine>
#include <mutex>
#include <print>
#include <thread>
#include <utility>

#include <unistd.h>

#include <gtest/gtest.h>

#include "x23/corotrace.h"
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
  std::atomic_uint nthreads = 0x1000'0000;

  SharedLock lock;
  lock.lock();
  std::jthread writer([&] {
    usleep(50'000);
    nthreads = 0;
    lock.unlock();
  });

  std::array<std::jthread, 64> readers;
  auto go = [&] {
    auto holder = lock.hold_shared();
    unsigned int n = ++nthreads;
    ASSERT_LE(n, readers.size());
    if (n == readers.size())
      nthreads.notify_all();
    else
      do {
        nthreads.wait(n);
      } while ((n = nthreads) != readers.size());
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

TEST(CoroSem, MutualExclusion)
{
  [] -> Async<void> {
    std::atomic_bool ready{false};
    int n = 0;
    CoroSem sem;
    [](std::atomic_bool &ready, int &n, CoroSem &sem) -> Detached {
      co_await pool.bg();
      co_await sem.acquire();
      ready = true;
      ready.notify_all();
      usleep(50'000);
      n = 1;
      sem.release();
    }(ready, n, sem);
    ready.wait(false);
    co_await sem.acquire();
    EXPECT_EQ(n, 1);
    sem.release();
    co_return;
  }()();
}

TEST(CoroSem, ManyWaiters)
{
  [] -> Async<void> {
    CoroSem sem;
    co_await sem.acquire();
    std::atomic_int lastid = -1;
    std::atomic_int waiting{0};

    for (int i = 0; i < 64; ++i)
      [](CoroSem &sem, std::atomic_int &lastid, int myid,
         std::atomic_int &waiting) -> Detached {
        co_await pool.bg();
        waiting.fetch_add(1, std::memory_order_relaxed);
        waiting.notify_all();
        co_await sem.acquire();
        EXPECT_GE(lastid, 0);
        EXPECT_NE(lastid, myid);
        lastid = myid;
        usleep(2'000);
        EXPECT_EQ(lastid, myid);
        sem.release();
      }(sem, lastid, i, waiting);

    while (waiting.load(std::memory_order_relaxed) < 64)
      waiting.wait(waiting.load(std::memory_order_relaxed));
    lastid = 0x1'0000;
    sem.release();
    co_await pool.async(&ThreadPool::drain, &pool);
  }()();
}

// Reproducer for the rmw→broadcast gap race (#1039).
// Widens the gap between rmw() and broadcast() so the Async exits
// and destroys futex before the Detached calls broadcast().
// Run under Clang ASAN (detect_stack_use_after_return=1) to trigger.
TEST(CoroFutex, DISABLED_DrainRace)
{
  std::atomic_bool rmw_done{false};
  x23::CoroFutex<int> futex{99};

  [&](this auto) -> Detached {
    co_await pool.bg();
    futex.rmw([](auto &i) { ++i; });
    rmw_done.store(true, std::memory_order_release);
    rmw_done.notify_all();
    usleep(50'000);
    futex.broadcast();
  }();

  rmw_done.wait(false);

  [&futex] -> Async<void> {
    int n;
    while ((n = futex.load()) < 100)
      co_await futex.wait(n);
  }()();
}

TEST(CoroFutex, Basic)
{
  [] -> Async<void> {
    x23::CoroFutex<int> futex{0};

    for (int i = 0; i < 50; ++i) {
      futex.rmw([](auto &i) { ++i; });
      [&futex](this auto self) -> Detached {
        int n;
        while ((n = futex.load()) < 50)
          co_await futex.wait(n);
        if (futex.rmw([](auto &i) { return ++i; }) == 100)
          futex.broadcast();
      }();
    }
    futex.broadcast(&pool);
    [&futex] -> Async<void> {
      int n;
      while ((n = futex.load()) < 100)
        co_await futex.wait(n);
    }()();
    co_await pool.async(&ThreadPool::drain, &pool);
  }()();
}

struct TooBigForAtomic {
  uint64_t v;
  char buf[248];
  friend bool operator==(const TooBigForAtomic &,
                         const TooBigForAtomic &) = default;
};
static_assert(!std::atomic<TooBigForAtomic>::is_always_lock_free);

TEST(CoroFutex, BigValue)
{
  [] -> Async<void> {
    x23::CoroFutex<TooBigForAtomic> futex{};

    for (int i = 0; i < 50; ++i) {
      futex.rmw([](auto &n) { ++n.v; });
      [&futex](this auto self) -> Detached {
        TooBigForAtomic n;
        while ((n = futex.load()).v < 50)
          co_await futex.wait(n);
        if (futex.rmw([](auto &n) { return ++n.v; }) == 100)
          futex.broadcast();
      }();
    }
    futex.broadcast(&pool);
    [&futex] -> Async<void> {
      TooBigForAtomic n;
      while ((n = futex.load()).v < 100)
        co_await futex.wait(n);
    }()();
    co_await pool.async(&ThreadPool::drain, &pool);
  }()();
}

namespace {

struct HandoffTask {
  struct promise_type : PromiseBase {
    promise_type() : PromiseBase(std::source_location::current()) {}
    HandoffTask get_return_object()
    {
      return HandoffTask{
          std::coroutine_handle<promise_type>::from_promise(*this)};
    }
    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }
    void return_void() noexcept {}
    void unhandled_exception() { std::terminate(); }
  };

  explicit HandoffTask(std::coroutine_handle<promise_type> h) : h_(h) {}
  HandoffTask(HandoffTask &&other) noexcept
    : h_(std::exchange(other.h_, {}))
  {}
  HandoffTask(const HandoffTask &) = delete;
  ~HandoffTask()
  {
    if (h_)
      h_.destroy();
  }

  void start() { h_.resume(); }

private:
  std::coroutine_handle<promise_type> h_;
};

struct FrameMarkerAwaiter {
  static constexpr int kPublished = 0x5a17;

  AtomicCoroHandle &slot_;
  int marker_ = 0;

  bool await_ready() const noexcept { return false; }
  void await_suspend(CoroHandle h) noexcept
  {
    marker_ = kPublished;
    slot_.put(h);
  }
  int await_resume() const noexcept { return marker_; }
};

HandoffTask
atomic_handle_handoff_probe(AtomicCoroHandle &slot, std::atomic_int &observed,
                            std::atomic_bool &complete)
{
  int marker = co_await FrameMarkerAwaiter{slot};
  observed.store(marker, std::memory_order_relaxed);
  complete.store(true, std::memory_order_release);
  complete.notify_all();
}

} // namespace

TEST(AtomicCoroHandle, RelaxedHandoffPublishesFrameWithoutSynchronization)
{
  AtomicCoroHandle slot;
  std::atomic_int observed{0};
  std::atomic_bool complete{false};

  HandoffTask task = atomic_handle_handoff_probe(slot, observed, complete);
  std::jthread resumer([&] {
    while (!slot)
      std::this_thread::yield();
    CoroHandle h = slot.take();
    EXPECT_TRUE(bool{h});
    h();
  });

  task.start();
  while (!complete.load(std::memory_order_acquire))
    complete.wait(false);

  EXPECT_EQ(observed.load(std::memory_order_relaxed),
            FrameMarkerAwaiter::kPublished);
}
