#include <atomic>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <memory>
#include <mutex>

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
