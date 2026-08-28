// GTest for Detached coroutine lifetime semantics.
//
// Verifies the construct/move/destroy-resumes contract and that
// parameters survive cross-thread suspend/resume.  Also covers
// Async<R>::operator()(cb).

#include <atomic>
#include <cstdint>
#include <semaphore>
#include <thread>
#include <utility>

#include <gtest/gtest.h>

#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/event.h"
#include "x23/threadpool.h"

using namespace x23;

ThreadPool pool;
CoroPoll cp(pool);

// Body runs when the Detached temporary is destroyed (fire-and-forget).
TEST(Detached, BodyRunsOnDestruction)
{
  [] -> Async<void> {
    std::atomic_bool ran{false};

    [](std::atomic_bool &ran) -> Detached {
      ran.store(true, std::memory_order_relaxed);
      co_return;
    }(ran);

    // Body should have run synchronously during ~Detached of the temporary.
    EXPECT_TRUE(ran.load(std::memory_order_relaxed));
    co_return;
  }()();
}

// Move transfers ownership: moved-from Detached does not resume.
TEST(Detached, MoveTransfersOwnership)
{
  [] -> Async<void> {
    std::atomic_int count{0};

    auto make = [](std::atomic_int &count) -> Detached {
      count.fetch_add(1, std::memory_order_relaxed);
      co_return;
    };

    // Capture the Detached before it's destroyed.
    Detached d = make(count);
    // Body hasn't started yet (initial_suspend = suspend_always).
    EXPECT_EQ(count.load(std::memory_order_relaxed), 0);

    // Move: d2 now owns the coroutine handle.
    Detached d2 = std::move(d);
    EXPECT_EQ(count.load(std::memory_order_relaxed), 0);

    // Destroy d2 — this should resume the body.
    {
      Detached sink = std::move(d2);
    }
    EXPECT_EQ(count.load(std::memory_order_relaxed), 1);

    co_return;
  }()();
}

// Body doesn't start until ~Detached when the return value is captured.
TEST(Detached, DeferredStart)
{
  [] -> Async<void> {
    std::atomic_int phase{0};

    Detached d = [](std::atomic_int &phase) -> Detached {
      phase.store(1, std::memory_order_relaxed);
      co_return;
    }(phase);

    // Coroutine suspended at initial_suspend — body hasn't run.
    EXPECT_EQ(phase.load(std::memory_order_relaxed), 0);

    // Explicitly destroy — body runs now.
    {
      Detached sink = std::move(d);
    }
    EXPECT_EQ(phase.load(std::memory_order_relaxed), 1);

    co_return;
  }()();
}

// Parameters survive cross-thread suspend/resume via pool.bg().
TEST(Detached, ParametersSurviveSuspend)
{
  [] -> Async<void> {
    constexpr uint64_t kMagic1 = 0xDEADBEEFCAFEBABE;
    constexpr uint64_t kMagic2 = 0x0123456789ABCDEF;
    std::atomic_bool done{false};
    std::atomic_bool ok{true};

    [](uint64_t m1, uint64_t m2, std::atomic_bool &ok,
       std::atomic_bool &done) -> Detached {
      co_await pool.bg();
      if (m1 != 0xDEADBEEFCAFEBABE || m2 != 0x0123456789ABCDEF)
        ok.store(false, std::memory_order_relaxed);
      done.store(true, std::memory_order_release);
      done.notify_all();
    }(kMagic1, kMagic2, ok, done);

    while (!done.load(std::memory_order_acquire))
      done.wait(false);
    EXPECT_TRUE(ok.load(std::memory_order_relaxed));
    co_return;
  }()();
}

// Many Detached coroutines, each with a unique ID, all resume on
// background threads.  Verifies no frame corruption under concurrency.
TEST(Detached, ManyCoroutinesCrossThread)
{
  [] -> Async<void> {
    constexpr int kN = 256;
    std::atomic_int completed{0};

    for (int i = 0; i < kN; i++) {
      [](int id, int expect_n, std::atomic_int &completed) -> Detached {
        // Verify parameter survived into the body.
        EXPECT_GE(id, 0);
        EXPECT_LT(id, expect_n);
        co_await pool.bg();
        // Verify parameter survived the cross-thread resume.
        EXPECT_GE(id, 0);
        EXPECT_LT(id, expect_n);
        completed.fetch_add(1, std::memory_order_relaxed);
      }(i, kN, completed);
    }

    co_await pool.async(&ThreadPool::drain, &pool);
    EXPECT_EQ(completed.load(std::memory_order_relaxed), kN);
    co_return;
  }()();
}

// drain() guarantees all enqueued jobs complete before it returns.
// Tight stress loop to catch the idle→executing race window.
TEST(DrainContract, AllJobsCompleteBeforeDrainReturns)
{
  [] -> Async<void> {
    constexpr int kN = 256;
    constexpr int kIters = 100;

    for (int iter = 0; iter < kIters; iter++) {
      std::atomic_int completed{0};

      for (int i = 0; i < kN; i++)
        [](std::atomic_int &completed) -> Detached {
          co_await pool.bg();
          completed.fetch_add(1, std::memory_order_relaxed);
        }(completed);

      co_await pool.async(&ThreadPool::drain, &pool);
      EXPECT_EQ(completed.load(std::memory_order_relaxed), kN);
    }
    co_return;
  }()();
}

// Async<R>::operator()(cb) has the same ramp hazard Detached exists to
// avoid, so it spawns a Detached rather than being a CallbackPromise
// coroutine itself.  A CallbackPromise has a suspend_never
// initial_suspend, so its body would run inside the ramp function; the
// body suspends awaiting the Async, and the thread that completes the
// Async then resumes it and -- final_suspend being suspend_never too --
// destroys the frame while the ramp is still writing to it.  TSAN
// reported that as a data race on the frame's suspend-point index at the
// closing brace of Async::operator().
//
// The window is narrow -- it closes when the ramp returns -- so a
// completer thread spins on [armed] and fires the moment the main thread
// enters operator().  A queued job (pool.bg()) lands too late to catch
// it.  Under TSAN this fails on the unfixed code within a few hundred
// iterations.
TEST(AsyncCallback, CompletionRacesTheRamp)
{
  constexpr int kIters = 5000;

  std::atomic_bool armed{false};
  CoroCB<int> pending;
  std::binary_semaphore go{0}, done{0};

  std::jthread completer([&](std::stop_token stop) {
    while (!stop.stop_requested()) {
      go.acquire();
      if (stop.stop_requested())
        return;
      while (!armed.load(std::memory_order_acquire))
        ; // spin so the completion lands inside the ramp, not after it
      pending(7);
    }
  });

  // Declared after [completer] so it runs first: the ASSERT below returns
  // from the test on failure, and ~jthread's request_stop() cannot wake a
  // semaphore wait, so without this the join would hang and CI would time
  // out instead of reporting the failure.  [go] is always 0 here -- the
  // done.acquire() that ends each iteration happens after the completer
  // consumed its token -- so exactly one release is owed.
  Defer wake_completer{[&] {
    completer.request_stop();
    go.release();
  }};

  int got = 0;
  for (int i = 0; i < kIters; i++) {
    auto [async, cb] = make_async<int>();
    pending = cb;
    armed.store(false, std::memory_order_relaxed);
    go.release();
    armed.store(true, std::memory_order_release);

    std::move(async)([&got, &done](int n) {
      got = n;
      done.release();
    });

    done.acquire();
    ASSERT_EQ(got, 7) << "iteration " << i;
  }
}

// Same race for an Async<void>, whose callback takes no argument.
TEST(AsyncCallback, VoidCompletionRacesTheRamp)
{
  constexpr int kIters = 5000;

  std::atomic_bool armed{false};
  CoroCB<void> pending;
  std::binary_semaphore go{0}, done{0};

  std::jthread completer([&](std::stop_token stop) {
    while (!stop.stop_requested()) {
      go.acquire();
      if (stop.stop_requested())
        return;
      while (!armed.load(std::memory_order_acquire))
        ;
      pending();
    }
  });

  // As above: teardown must run even if the test exits early.
  Defer wake_completer{[&] {
    completer.request_stop();
    go.release();
  }};

  for (int i = 0; i < kIters; i++) {
    auto [async, cb] = make_async<void>();
    pending = cb;
    armed.store(false, std::memory_order_relaxed);
    go.release();
    armed.store(true, std::memory_order_release);

    std::move(async)([&done] { done.release(); });

    done.acquire();
  }
}

// Multiple suspend points per coroutine — parameters intact after each.
TEST(Detached, MultipleSuspendPoints)
{
  [] -> Async<void> {
    constexpr uint64_t kCanary = 0xA5A5A5A5A5A5A5A5;
    std::atomic_bool done{false};
    std::atomic_bool ok{true};

    [](uint64_t canary, std::atomic_bool &ok,
       std::atomic_bool &done) -> Detached {
      for (int i = 0; i < 10; i++) {
        co_await pool.bg();
        if (canary != 0xA5A5A5A5A5A5A5A5) {
          ok.store(false, std::memory_order_relaxed);
          break;
        }
      }
      done.store(true, std::memory_order_release);
      done.notify_all();
    }(kCanary, ok, done);

    while (!done.load(std::memory_order_acquire))
      done.wait(false);
    EXPECT_TRUE(ok.load(std::memory_order_relaxed));
    co_return;
  }()();
}
