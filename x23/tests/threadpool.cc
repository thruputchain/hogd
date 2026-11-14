#include <atomic>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "x23/async.h"
#include "x23/cleanup.h"
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

namespace {

// An async() argument whose destructor reaches back into the pool.  The
// helper thread runs that destructor, so it has to still be counted when it
// does -- destroying the captured arguments is part of the helper touching
// the pool, not something that happens after it is done.  The sleep puts
// the destructor squarely inside the window where the awaiting coroutine
// has already resumed and the main thread is in ~ThreadPool.
struct TouchesPoolWhenDestroyed {
  ThreadPool *pool_;

  ~TouchesPoolWhenDestroyed() // NOLINT(bugprone-exception-escape)
  {
    std::this_thread::sleep_for(microseconds(500));
    pool_->on_quiesce([]() noexcept {});
  }
};

} // namespace

// async() keeps the helper counted until the closure's captures have been
// destroyed, not merely until its body returns: captures are destroyed
// afterwards, and a destructor of f or of an argument can reach the pool.
//
// This covers the other half of that contract -- that holding the count
// across capture destruction does not deadlock.  ~ThreadPool now waits on
// the count, so a capture destructor that re-enters the pool (here
// on_quiesce, which takes a lock the destructor is about to destroy) is
// running while the pool's own destructor is blocked waiting for it.
//
// It is not a discriminating regression test for the lifetime bug itself:
// with the count dropped too early the late destructor lands on whatever
// occupies the pool's storage next, which on this loop is the following
// iteration's pool at the same address, so nothing observably breaks.
TEST(ThreadPoolAsync, CaptureDestructorMayReenterPool)
{
  for (unsigned i = 0; i < 50; ++i) {
    ThreadPool pool(2);
    std::atomic_bool awaited{false};

    [](ThreadPool &pool, std::atomic_bool &awaited) -> Detached {
      co_await pool.async([](const TouchesPoolWhenDestroyed &) {},
                          TouchesPoolWhenDestroyed{&pool});
      awaited.store(true, std::memory_order_release);
      awaited.notify_all();
    }(pool, awaited);

    while (!awaited.load(std::memory_order_acquire))
      awaited.wait(false, std::memory_order_relaxed);
  }
}

// Keep cleanup blocked after result delivery so teardown must wait for
// it.  Nested async() also requires workers to remain available during
// the wait, and registers another helper while destruction is pending.
TEST(ThreadPoolAsync, WaitsForCaptureDestructorReentry)
{
  auto pool = std::make_unique<ThreadPool>(2);
  std::atomic_bool cleanup_started{false};
  std::atomic_bool release_cleanup{false};
  std::mutex mutex;
  std::condition_variable changed;
  bool destroying = false;
  bool destroyed = false;

  Defer cleanup{[tp = pool.get(), &cleanup_started, &release_cleanup] {
    cleanup_started = true;
    cleanup_started.notify_all();
    release_cleanup.wait(false);
    EXPECT_FALSE(in_thread_pool());
    auto work = [](ThreadPool &pool) -> Async<int> {
      co_await pool.bg();
      co_return 17;
    };
    EXPECT_EQ(tp->async([tp, work] { return work(*tp)(); })(), 17);
  }};
  EXPECT_EQ(pool->async([](const auto &) { return 42; }, std::move(cleanup))(),
            42);
  cleanup_started.wait(false);

  std::jthread destroyer([&] {
    {
      std::lock_guard lock(mutex);
      destroying = true;
      changed.notify_all();
    }
    pool.reset();
    {
      std::lock_guard lock(mutex);
      destroyed = true;
      changed.notify_all();
    }
  });
  {
    std::unique_lock lock(mutex);
    changed.wait(lock, [&] { return destroying; });
    EXPECT_FALSE(
        changed.wait_for(lock, milliseconds(100), [&] { return destroyed; }));
  }
  release_cleanup = true;
  release_cleanup.notify_all();
  destroyer.join();
}

// A job can finish as soon as its submitter drops the queue lock.
// Destruction must still wait for that submitter's notify_all() to
// return before freeing the condition variable.  Without lifetime
// protection TSAN reports pthread_cond_broadcast racing with
// pthread_cond_destroy.
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

TEST(ThreadPool, DestructionRacesBatchEnqueueWakeup)
{
  constexpr unsigned kJobs = 8;
  for (unsigned i = 0; i < 500; ++i) {
    std::atomic_uint jobs_ran{0};
    auto pool = std::make_unique<ThreadPool>(4);

    std::thread submitter([tp = pool.get(), &jobs_ran] {
      std::vector<ThreadPool::Job> jobs;
      jobs.reserve(kJobs);
      // The compiler invokes Suspended's static promise hooks.
      for (unsigned j = 0; j < kJobs; ++j)
        // NOLINTNEXTLINE(readability-static-accessed-through-instance)
        jobs.emplace_back([](std::atomic_uint &done) -> Suspended {
          done.fetch_add(1, std::memory_order_release);
          done.notify_all();
          co_return;
        }(jobs_ran));
      tp->go(std::move(jobs));
    });

    for (unsigned n; (n = jobs_ran.load(std::memory_order_acquire)) != kJobs;)
      jobs_ran.wait(n, std::memory_order_relaxed);
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

#if HAVE_SCHED_SETAFFINITY
TEST(ThreadPool, PinnedWorkersRunOnlyOnTheirCpus)
{
  // One allowed CPU, so that a worker off it would be caught at once.
  CpuSet cpus;
  for (unsigned cpu = 0; cpu < CpuSet::num_cpus(); cpu++)
    if (allowed_cpus->isset(cpu)) {
      cpus.set(cpu);
      break;
    }
  ASSERT_EQ(cpus.count(), 1);

  constexpr int kJobs = 1000;
  std::atomic_int done{0};
  std::atomic_int off_cpu{0};
  {
    ThreadPool pool(4, cpus);
    for (int i = 0; i < kJobs; i++)
      pool.spawn([&] {
        if (!cpus.isset(cpuid()))
          off_cpu++;
        done++;
      });
    for (auto deadline = steady_clock::now() + 30s;
         done < kJobs && steady_clock::now() < deadline;)
      std::this_thread::sleep_for(1ms);
  }
  EXPECT_EQ(done, kJobs);
  EXPECT_EQ(off_cpu, 0);
}
#endif // HAVE_SCHED_SETAFFINITY
