#include <atomic>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

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

TEST(ThreadPoolWake, ParallelJobs)
{
  for (bool batch : {false, true}) {
    std::mutex mutex;
    std::condition_variable ready;
    std::condition_variable release;
    unsigned started = 0;
    bool released = false;
    ThreadPool pool(32);
    std::vector<ThreadPool::Job> jobs;
    std::vector<CoroCB<void>> callbacks;

    for (unsigned i = 0; i < 8; ++i) {
      auto [pending, cb] = make_async<void>();
      [](Async<void> pending, std::mutex &mutex, std::condition_variable &ready,
         std::condition_variable &release, unsigned &started,
         bool &released) -> Detached {
        co_await pending;
        std::unique_lock lock(mutex);
        ++started;
        ready.notify_one();
        release.wait(lock, [&] { return released; });
      }(std::move(pending), mutex, ready, release, started, released);
      callbacks.push_back(cb);
    }
    for (auto &cb : callbacks) {
      auto h = cb.set_result_without_resuming();
      ASSERT_TRUE(h);
      if (batch)
        jobs.emplace_back(h);
      else
        pool.go(ThreadPool::Job{h});
    }
    if (batch)
      pool.go(std::move(jobs));

    {
      std::unique_lock lock(mutex);
      // Keep the first jobs busy so a missing notification cannot be
      // hidden by one worker draining the whole queue sequentially.
      EXPECT_TRUE(
          ready.wait_for(lock, seconds(5), [&] { return started == 8; }));
      released = true;
      release.notify_all();
    }
    pool.drain();
  }
}
