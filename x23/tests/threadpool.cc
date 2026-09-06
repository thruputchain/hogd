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

// A job enqueued on an empty runqueue wakes a worker on that queue's
// `nonempty` condition variable, and the notify has to happen before
// the queue lock is dropped.  The lock is what keeps a worker from
// popping the job, so once it is dropped the job can run, and running
// the job is what tells the rest of the program the pool is idle and
// may be destroyed.  Signalling after the unlock instead lets
// ~ThreadPool free the condition variable while the enqueuing thread
// is still inside notify_all() -- reported by TSAN as a race between
// pthread_cond_broadcast and pthread_cond_destroy.
//
// The submit has to come from a thread other than the one destroying
// the pool, or the submitter's own notify_all() is trivially ordered
// before the destructor; hence the deliberately late join().  Unlike
// DestructionWakeupStress this needs a real worker thread, since
// on_quiesce never touches a runqueue.  The window is a few
// instructions wide, so iterate to make hitting it likely.
TEST(ThreadPool, DestructionRacesEnqueueWakeup)
{
  for (unsigned i = 0; i < 500; ++i) {
    std::atomic_bool job_ran{false};
    auto pool = std::make_unique<ThreadPool>(1);

    std::thread submitter([tp = pool.get(), &job_ran] {
      tp->spawn([&job_ran]() noexcept {
        job_ran.store(true, std::memory_order_release);
        job_ran.notify_all();
      });
    });

    job_ran.wait(false, std::memory_order_acquire);
    pool.reset();
    submitter.join();
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
