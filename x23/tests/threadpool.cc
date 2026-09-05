#include <atomic>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include <gtest/gtest.h>

#include "x23/move_only_function.h"
#include "x23/threadpool.h"

using namespace std::chrono;
using namespace x23;

static_assert(std::constructible_from<move_only_function<void() noexcept>,
                                      decltype([]() noexcept {})>);
static_assert(!std::constructible_from<move_only_function<void() noexcept>,
                                       decltype([] {})>);

TEST(ThreadPoolOnQuiesce, WaitsForSubmittingWorker)
{
  std::mutex mutex;
  std::condition_variable callback_ready;
  bool callback_ran = false;
  std::atomic_bool worker_started{false};
  std::atomic_bool release_worker{false};
  ThreadPool pool(1);

  pool.spawn([&] {
    pool.on_quiesce([&]() noexcept {
      std::lock_guard lock(mutex);
      callback_ran = true;
      callback_ready.notify_one();
    });
    worker_started = true;
    worker_started.notify_one();
    release_worker.wait(false);
  });

  worker_started.wait(false);
  {
    std::unique_lock lock(mutex);
    EXPECT_FALSE(callback_ready.wait_for(lock, milliseconds(100),
                                         [&] { return callback_ran; }));
  }

  release_worker = true;
  release_worker.notify_one();
  {
    std::unique_lock lock(mutex);
    EXPECT_TRUE(callback_ready.wait_for(lock, seconds(5),
                                        [&] { return callback_ran; }));
  }
}

TEST(ThreadPoolOnQuiesce, SupportsMoveOnlyCallbacks)
{
  std::atomic_int value_seen{0};
  std::atomic_bool done{false};
  ThreadPool pool(2);

  pool.on_quiesce([&, value = std::make_unique<int>(17)]() noexcept {
    value_seen = *value;
    done = true;
    done.notify_one();
  });

  done.wait(false);
  EXPECT_EQ(value_seen, 17);
}

TEST(ThreadPoolOnQuiesce, DestructionRunsPendingCallbacks)
{
  std::atomic_int callbacks{0};
  {
    ThreadPool pool(1);
    pool.on_quiesce([&]() noexcept { ++callbacks; });
  }
  EXPECT_EQ(callbacks, 1);
}

TEST(ThreadPoolOnQuiesce, DestructionWakeupStress)
{
  for (unsigned i = 0; i < 10000; ++i) {
    std::atomic_bool callback_ran{false};
    auto pool = std::make_unique<ThreadPool>(0);

    pool->on_quiesce([&]() noexcept {
      callback_ran.store(true, std::memory_order_release);
    });
    while (!callback_ran.load(std::memory_order_acquire))
      std::this_thread::yield();

    pool.reset();
  }
}

TEST(ThreadPoolOnQuiesce, RepeatedWakeups)
{
  constexpr unsigned kCoroutines = 64;
  std::atomic_bool stop{false};
  std::atomic_uint done{0};
  ThreadPool pool(8);

  for (unsigned i = 0; i < kCoroutines; ++i)
    [](ThreadPool &pool, std::atomic_bool &stop,
       std::atomic_uint &done) -> Detached {
      do {
        co_await pool.bg();
      } while (!stop.load(std::memory_order_relaxed));
      done.fetch_add(1, std::memory_order_release);
      done.notify_all();
    }(pool, stop, done);

  for (unsigned i = 0; i < 10'000; ++i)
    pool.quiesce();

  stop.store(true, std::memory_order_relaxed);
  for (unsigned count = done.load(std::memory_order_acquire);
       count != kCoroutines; count = done.load(std::memory_order_acquire))
    done.wait(count, std::memory_order_relaxed);
}

// Regression test for #3865.  ThreadPool::async() runs its callable on a
// detached thread that calls back into go() to resume the awaiting
// coroutine, and go() is only allowed to signal the run queue while it
// still holds that queue's lock.  Signalling after the unlock lets the
// resumed coroutine -- and hence everything the resumption unblocks,
// including destroying the pool -- overtake go()'s last touch of the
// queue's condition variable, which TSAN reports as pthread_cond_destroy
// racing pthread_cond_broadcast.  The pool is destroyed as soon as the
// await returns so the window is as narrow as it can be.
TEST(ThreadPoolAsync, ResumptionDoesNotOutraceEnqueue)
{
  for (unsigned i = 0; i < 200; ++i) {
    auto pool = std::make_unique<ThreadPool>(2);
    std::atomic_bool resumed{false};

    [](ThreadPool &pool, std::atomic_bool &resumed) -> Detached {
      co_await pool.async([] {});
      resumed.store(true, std::memory_order_release);
      resumed.notify_all();
    }(*pool, resumed);

    while (!resumed.load(std::memory_order_acquire))
      resumed.wait(false);
    pool.reset();
  }
}
