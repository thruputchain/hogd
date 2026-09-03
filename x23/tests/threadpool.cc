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

// async() runs its task on a detached thread, so the pool's own bookkeeping
// of its worker threads says nothing about when that thread is done.  The
// helper hands the result back from inside go(), which resumes the awaiting
// coroutine on a worker before it has finished notifying the run queue it
// enqueued on.  So the awaiting coroutine can run to completion, and the
// pool can be destroyed, while the helper is still touching a run queue --
// ~ThreadPool has to wait for the helper, not just join its workers.  Under
// TSAN the unfixed version reports ~ThreadPool's pthread_cond_destroy
// racing with the helper's notify_all.
TEST(ThreadPoolAsync, HelperOutlivesAwaitingCoroutine)
{
  for (unsigned i = 0; i < 500; ++i) {
    ThreadPool pool(2);
    std::atomic_bool awaited{false};

    [](ThreadPool &pool, std::atomic_bool &awaited) -> Detached {
      co_await pool.async([] {});
      awaited.store(true, std::memory_order_release);
      awaited.notify_all();
    }(pool, awaited);

    while (!awaited.load(std::memory_order_acquire))
      awaited.wait(false, std::memory_order_relaxed);
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
