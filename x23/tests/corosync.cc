#include <array>
#include <atomic>
#include <barrier>
#include <coroutine>
#include <memory>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/corosync.h"
#include "x23/testlock.h"
#include "x23/threadpool.h"

using namespace x23;

namespace {

template<bool Prefer, typename T>
Async<T>
return_locked(CoroLock &lock, Awaitable<void> &gate, int &destroyed)
{
  Defer cleanup{[&] { ++destroyed; }};
  CoroGuard guard;
  co_await guard(lock);
  co_await gate;
  if constexpr (std::is_void_v<T>) {
    if constexpr (Prefer)
      co_await guard.reset_and_return_prefer_caller();
    else
      co_await guard.reset_and_return();
  }
  else {
    if constexpr (Prefer)
      co_await guard.reset_and_return_prefer_caller(std::make_unique<int>(42));
    else
      co_await guard.reset_and_return(std::make_unique<int>(42));
  }
  std::unreachable();
}

template<bool Prefer, typename T>
void
check_return_handoff()
{
  for (bool waiting_caller : {false, true})
    for (bool waiting_locker : {false, true}) {
      CoroLock lock;
      Awaitable<void> gate;
      CoroCB release{gate};
      int destroyed = 0;
      {
        auto task = return_locked<Prefer, T>(lock, gate, destroyed);
        EXPECT_TRUE(lock.busy());
        std::vector<int> events;
        if (waiting_locker)
          [&](this auto) -> Detached {
            CoroGuard guard;
            co_await guard(lock);
            events.push_back(2);
          }();
        if (waiting_caller)
          [&](this auto) -> Detached {
            if constexpr (std::is_void_v<T>)
              co_await task;
            else
              EXPECT_EQ(*(co_await task), 42);
            events.push_back(1);
          }();
        EXPECT_TRUE(events.empty());
        release();
        EXPECT_FALSE(lock.busy());
        if (!waiting_caller) {
          if constexpr (std::is_void_v<T>)
            task();
          else
            EXPECT_EQ(*task(), 42);
        }
        if (waiting_caller && waiting_locker)
          // With no thread pool, the other continuation runs inline.
          EXPECT_EQ(events, Prefer ? (std::vector{2, 1}) : (std::vector{1, 2}));
        else if (waiting_caller)
          EXPECT_EQ(events, (std::vector{1}));
        else if (waiting_locker)
          EXPECT_EQ(events, (std::vector{2}));
        else
          EXPECT_TRUE(events.empty());
        EXPECT_EQ(destroyed, 0);
      }
      EXPECT_EQ(destroyed, 1);
    }
}

struct InhibitedLock : CoroLock {
  int calls_ = 0;
  void block_while_inhibited()
  {
    EXPECT_EQ(ThreadPool::get_thread_pool(), nullptr);
    ++calls_;
  }
};

struct BlockingLock : TestLock {
  std::binary_semaphore waiting_{0};
  void block_until_free()
  {
    waiting_.release();
    TestLock::block_until_free();
  }
};

struct SharedBase : TestLock {
  int calls_ = 0;
  void block_while_inhibited() { ++calls_; }
};

struct BigValue {
  std::array<int, 64> data_{};
  bool operator==(const BigValue &) const = default;
};
static_assert(!std::atomic<BigValue>::is_always_lock_free);

} // namespace

TEST(CoroLock, QueueOrderGuardReuseAndAwaitFree)
{
  CoroLock lock;
  CoroGuard owner;
  EXPECT_FALSE(lock.busy());
  auto acquire = [&] -> Async<void> { co_await owner(lock); }();
  acquire();
  EXPECT_TRUE(lock.busy());
  std::vector<int> events;
  for (int i = 0; i < 4; ++i)
    [&, i](this auto) -> Detached {
      CoroGuard guard;
      co_await guard(lock);
      events.push_back(i);
    }();
  auto free = lock.await_free();
  EXPECT_TRUE(events.empty());
  owner.reset();
  free();
  EXPECT_EQ(events, (std::vector{0, 1, 2, 3}));
  EXPECT_FALSE(lock.busy());
  owner.reset();
  owner.reset_no_yield();
  [&] -> Async<void> { co_await owner(lock); }()();
  owner.reset_no_yield(nullptr);
  EXPECT_FALSE(lock.busy());
}

TEST(CoroLock, NoYieldResumesInlineOrInRequestedPool)
{
  ThreadPool pool(4);
  for (auto *target : {static_cast<ThreadPool *>(nullptr), &pool}) {
    CoroLock lock;
    CoroGuard owner;
    [&] -> Async<void> { co_await owner(lock); }()();
    auto task = [&](this auto) -> Async<ThreadPool *> {
      CoroGuard guard;
      co_await guard(lock);
      co_return ThreadPool::get_thread_pool();
    }();
    owner.reset_no_yield(target);
    EXPECT_EQ(task(), target);
    EXPECT_FALSE(lock.busy());
  }
  pool.drain();
}

TEST(CoroLock, ReturnHandoffValueAndVoid)
{
  check_return_handoff<false, void>();
  check_return_handoff<false, std::unique_ptr<int>>();
  check_return_handoff<true, void>();
  check_return_handoff<true, std::unique_ptr<int>>();
}

TEST(CoroLock, DetachedReturnDestroysBeforeHandoff)
{
  for (bool prefer : {false, true})
    for (bool queued : {false, true}) {
      CoroLock lock;
      Awaitable<void> gate;
      CoroCB release{gate};
      int destroyed = 0;
      bool resumed = false;
      [&](this auto) -> Detached {
        Defer cleanup{[&] { ++destroyed; }};
        CoroGuard guard;
        co_await guard(lock);
        co_await gate;
        if (prefer)
          co_await guard.reset_and_return_prefer_caller();
        else
          co_await guard.reset_and_return();
      }();
      if (queued)
        [&](this auto) -> Detached {
          CoroGuard guard;
          co_await guard(lock);
          EXPECT_EQ(destroyed, 1);
          resumed = true;
        }();
      release();
      EXPECT_EQ(destroyed, 1);
      EXPECT_EQ(resumed, queued);
      EXPECT_FALSE(lock.busy());
    }
}

TEST(CoroLock, DerivedInhibitionHookFromGuardAndDirectLock)
{
  InhibitedLock lock;
  CoroLock::LockerState state;
  lock.lock (&state)();
  EXPECT_EQ(lock.calls_, 1);
  lock.unlock(&state);
  [&] -> Async<void> {
    CoroGuard guard;
    co_await guard(lock);
    EXPECT_EQ(lock.calls_, 2);
  }()();
  ThreadPool pool(4);
  [&] -> Async<void> {
    co_await pool.bg();
    CoroGuard guard;
    co_await guard(lock);
    EXPECT_EQ(lock.calls_, 3);
  }()();
  pool.drain();
  EXPECT_FALSE(lock.busy());
}

TEST(CoroSharedLock, ReadersBeforeAndAfterExclusiveLock)
{
  ThreadPool pool(4);
  CoroSharedLock lock(pool);
  [&] -> Async<void> {
    co_await pool.bg();
    auto holder = co_await lock.coro_lock_shared();
    EXPECT_EQ(ThreadPool::get_thread_pool(), &pool);
    holder.reset();
    auto again = co_await lock.coro_lock_shared();
    again.reset();
  }()();
  CoroGuard owner;
  [&] -> Async<void> { co_await owner(lock); }()();
  // Drain waits until this reader has queued on the exclusive lock.
  auto reader = [&](this auto) -> Async<int> {
    auto holder = co_await lock.coro_lock_shared();
    co_return 23;
  }();
  pool.drain();
  // Shared holders must resume in their pool, including when an external
  // thread releases the writer's lock.
  owner.reset_no_yield(&pool);
  EXPECT_EQ(reader(), 23);
  pool.drain();
}

TEST(CoroSharedLock, BlockingBaseAndInheritedInhibitionHook)
{
  ThreadPool pool(4);
  CoroSharedLock<BlockingLock> lock(pool);
  lock.lock();
  auto reader = [&](this auto) -> Async<int> {
    auto holder = co_await lock.coro_lock_shared();
    co_return 31;
  }();
  lock.waiting_.acquire();
  lock.unlock();
  EXPECT_EQ(reader(), 31);
  pool.drain();

  CoroSharedLock<SharedBase> inherited(pool);
  inherited.lock();
  EXPECT_EQ(inherited.calls_, 1);
  inherited.unlock();
}

TEST(CoroTempPtr, AccessAssignmentAndResetAcrossSuspension)
{
  ThreadPool pool(4);
  [&] -> Async<void> {
    co_await pool.bg();
    struct Value {
      int n_;
    } first{7}, second{9};
    CoroTempPtr<Value> empty;
    EXPECT_FALSE(bool(empty));
    EXPECT_EQ(empty.get(), nullptr);
    CoroTempPtr ptr{&first};
    EXPECT_TRUE(bool(ptr));
    EXPECT_EQ(ptr.get(), &first);
    EXPECT_EQ(ptr->n_, 7);
    Value &ref = ptr;
    EXPECT_EQ(&ref, &first);
    ptr = &second;
    EXPECT_EQ(ptr->n_, 9);
    ThreadPool::CrashIfSuspended holder;
    CoroTempPtr shared{&first, holder};
    EXPECT_EQ(shared->n_, 7);
    shared.reset();
    ptr = nullptr;
    EXPECT_FALSE(bool(ptr));
    holder.reset();
    empty.reset();
    co_await pool.bg();
    EXPECT_FALSE(bool(shared));
    EXPECT_EQ(ptr.get(), nullptr);
    ptr = &first;
    EXPECT_EQ(ptr->n_, 7);
    ptr.reset();
  }()();
  pool.drain();
}

TEST(CoroSem, TokensFifoAndQueueReuse)
{
  CoroSem sem(2);
  auto take = [&] -> Async<void> { co_await sem.acquire(); };
  take()();
  take()();
  std::vector<int> events;
  for (int i = 0; i < 3; ++i)
    [&, i](this auto) -> Detached {
      co_await sem.acquire();
      events.push_back(i);
    }();
  EXPECT_TRUE(events.empty());
  for (int i = 0; i < 3; ++i) {
    sem.release();
    EXPECT_EQ(events.size(), i + 1UZ);
    EXPECT_EQ(events.back(), i);
  }
  sem.release();
  take()();
  auto queued = take();
  sem.release();
  queued();
  [&] -> Async<void> { co_await sem.release(); }()();
  take()();
}

TEST(CoroSem, RecheckingReadinessRetainsTheQueueLock)
{
  CoroSem sem(0);
  auto acquire = sem.acquire();
  EXPECT_FALSE(acquire.await_ready());
  EXPECT_FALSE(acquire.await_ready());
  bool resumed = false;
  auto continuation = [&](this auto) -> Suspended {
    resumed = true;
    co_return;
  }();
  acquire.await_suspend(continuation);
  EXPECT_FALSE(resumed);
  sem.release();
  EXPECT_TRUE(resumed);
  acquire.await_resume();
}

TEST(CoroFutex, AtomicValueConditionalWaitAndBroadcast)
{
  ThreadPool pool(4);
  CoroFutex<int> futex{1};
  EXPECT_EQ(futex.load(), 1);
  futex.store(2);
  EXPECT_EQ(futex.rmw([](int &v) { return v++; }), 2);
  EXPECT_EQ(futex.load(), 3);
  // rmw publishes changes even if the function throws.
  EXPECT_THROW(futex.rmw([](int &v) {
    v = 4;
    throw std::runtime_error("rmw error");
  }),
               std::runtime_error);
  EXPECT_EQ(futex.load(), 4);
  [&] -> Async<void> { co_await futex.wait(3); }()();
  std::atomic_int resumed = 0;
  auto wait = [&] -> Async<void> {
    co_await futex.wait(4);
    ++resumed;
  };
  auto one = wait(), two = wait();
  EXPECT_EQ(resumed, 0);
  futex.broadcast(&pool);
  one();
  two();
  EXPECT_EQ(resumed, 2);
  EXPECT_EQ(futex.load(), 4);
  futex.broadcast(&pool);
  pool.drain();
}

TEST(CoroFutex, NonAtomicValueAndUnconditionalWait)
{
  ThreadPool pool(4);
  CoroFutex<BigValue> futex;
  BigValue value;
  value.data_[0] = 8;
  futex.store(value);
  EXPECT_EQ(futex.load(), value);
  EXPECT_EQ(futex.rmw([](BigValue &v) { return ++v.data_[0]; }), 9);
  [&] -> Async<void> { co_await futex.wait(value); }()();
  value.data_[0] = 9;
  auto conditional = [&](this auto) -> Async<void> {
    co_await futex.wait(value);
  }();
  auto unconditional = [&](this auto) -> Async<void> {
    co_await futex.wait();
  }();
  futex.broadcast(&pool);
  conditional();
  unconditional();
  pool.drain();

  CoroFutex<> event;
  auto waiter = [&](this auto) -> Async<void> { co_await event.wait(); }();
  [&] -> Async<void> {
    co_await pool.bg();
    event.broadcast();
  }()();
  waiter();
  pool.drain();
}

TEST(CoroSem, ManyThreadsPreserveMutualExclusion)
{
  ThreadPool pool(16);
  CoroSem sem;
  int count = 0;
  std::vector<Async<void>> tasks;
  tasks.reserve(128);
  for (int i = 0; i < 128; ++i)
    tasks.push_back([&](this auto) -> Async<void> {
      for (int j = 0; j < 16; ++j) {
        co_await pool.bg();
        co_await sem.acquire();
        ++count;
        co_await sem.release();
      }
    }());
  for (auto &task : tasks)
    task();
  pool.drain();
  EXPECT_EQ(count, 128 * 16);
}

TEST(CoroLock, ConcurrentReturnHandoffs)
{
  ThreadPool pool(16);
  CoroLock lock;
  for (bool prefer : {false, true}) {
    int count = 0;
    auto increment = [&] -> Async<int> {
      CoroGuard guard;
      co_await guard(lock);
      int result = ++count;
      if (prefer)
        co_await guard.reset_and_return_prefer_caller(result);
      else
        co_await guard.reset_and_return(result);
      std::unreachable();
    };
    std::vector<Async<void>> tasks;
    tasks.reserve(128);
    for (int i = 0; i < 128; ++i)
      tasks.push_back([&](this auto) -> Async<void> {
        co_await pool.bg();
        int last = 0;
        for (int j = 0; j < 16; ++j) {
          int next = co_await increment();
          EXPECT_GT(next, last);
          last = next;
        }
      }());
    for (auto &task : tasks)
      task();
    pool.drain();
    EXPECT_EQ(count, 128 * 16);
    EXPECT_FALSE(lock.busy());
  }
}

namespace {

// Starts a coroutine that waits for [d] to drain, then sets [resumed].
void
start_drain_waiter(CoroDrain &d, bool &resumed)
{
  [](CoroDrain &d, bool &resumed) -> Detached {
    co_await d.wait_idle();
    resumed = true;
  }(d, resumed);
}

} // namespace

TEST(CoroDrain, IdleDrainDoesNotSuspend)
{
  CoroDrain d;
  bool resumed = false;
  start_drain_waiter(d, resumed);
  EXPECT_TRUE(resumed);
  EXPECT_EQ(d.in_flight(), 0U);
}

TEST(CoroDrain, LastDoneResumesWaiterInline)
{
  CoroDrain d;
  d.add();
  d.add();
  d.add();
  bool resumed = false;
  start_drain_waiter(d, resumed);
  EXPECT_FALSE(resumed);
  d.done();
  d.done();
  EXPECT_FALSE(resumed);
  EXPECT_EQ(d.in_flight(), 1U);
  d.done();
  // Resumed by the done() call itself, before it returned.
  EXPECT_TRUE(resumed);
  EXPECT_EQ(d.in_flight(), 0U);
}

TEST(CoroDrain, OperationsAddedWhileWaitingAreAwaited)
{
  CoroDrain d;
  d.add();
  bool resumed = false;
  start_drain_waiter(d, resumed);
  d.add();
  d.done();
  EXPECT_FALSE(resumed);
  d.done();
  EXPECT_TRUE(resumed);
}

TEST(CoroDrain, WaitAgainAfterADrain)
{
  CoroDrain d;
  for (int round = 0; round < 3; ++round) {
    d.add();
    bool resumed = false;
    start_drain_waiter(d, resumed);
    EXPECT_FALSE(resumed) << "round " << round;
    d.done();
    EXPECT_TRUE(resumed) << "round " << round;
  }
}

TEST(CoroDrain, OperationsAfterADrainResumeNothing)
{
  CoroDrain d;
  d.add();
  bool resumed = false;
  start_drain_waiter(d, resumed);
  d.done();
  ASSERT_TRUE(resumed);

  // Stragglers that start and finish with no waiter parked.
  d.add();
  d.done();
  d.add();
  d.add();
  d.done();
  d.done();

  bool resumed_again = false;
  start_drain_waiter(d, resumed_again);
  EXPECT_TRUE(resumed_again);
}

// The waiter may destroy the drain as soon as it resumes, while the
// done() that resumed it is still on the stack.
TEST(CoroDrain, WaiterMayDestroyDrainOnResume)
{
  auto *d = new CoroDrain;
  d->add();
  d->add();
  bool destroyed = false;
  [](CoroDrain *d, bool &destroyed) -> Detached {
    co_await d->wait_idle();
    delete d;
    destroyed = true;
  }(d, destroyed);
  d->done();
  EXPECT_FALSE(destroyed);
  d->done();
  EXPECT_TRUE(destroyed);
}

// Threads retire operations while the waiter parks before or races
// with them.  The waiter frees the drain on resume, so a done() that
// touched it after handing off, or a lost or doubled wakeup, shows up
// here (most reliably under ASan or TSan).
TEST(CoroDrain, ConcurrentDrainThenDestroy)
{
  constexpr int kIterations = 5000;
  constexpr int kThreads = 4;
  constexpr int kOpsPerThread = 32;

  CoroDrain *current = nullptr;
  bool stop = false;
  std::barrier start_line(kThreads + 1);
  std::vector<std::jthread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t)
    threads.emplace_back([&] {
      while (true) {
        start_line.arrive_and_wait();
        if (stop)
          return;
        CoroDrain *d = current;
        for (int i = 0; i < kOpsPerThread; ++i)
          d->done();
      }
    });

  std::binary_semaphore resumed{0};
  std::atomic<int> resumes{0};
  auto start_waiter = [&](CoroDrain *d) {
    [](CoroDrain *d, std::atomic<int> &resumes,
       std::binary_semaphore &resumed) -> Detached {
      co_await d->wait_idle();
      EXPECT_EQ(d->in_flight(), 0U);
      delete d;
      resumes.fetch_add(1);
      resumed.release();
    }(d, resumes, resumed);
  };

  for (int iter = 0; iter < kIterations; ++iter) {
    auto *d = new CoroDrain;
    for (int i = 0; i < kThreads * kOpsPerThread; ++i)
      d->add();
    current = d;
    if (iter % 2 == 0) {
      start_waiter(d);
      start_line.arrive_and_wait();
    }
    else {
      start_line.arrive_and_wait();
      start_waiter(d);
    }
    resumed.acquire();
    ASSERT_EQ(resumes.load(), iter + 1);
  }

  stop = true;
  start_line.arrive_and_wait();
}

// Operations keep starting and finishing, often from zero, while one
// waiter repeatedly drains: the count goes back up after each wait
// completes.  Every wait must still be woken, exactly once.
TEST(CoroDrain, ConcurrentRepeatedWaitsWithStragglers)
{
  constexpr int kWaits = 5000;
  constexpr int kThreads = 3;

  CoroDrain d;
  std::atomic<bool> stop{false};
  std::vector<std::jthread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t)
    threads.emplace_back([&d, &stop] {
      while (!stop.load(std::memory_order_relaxed)) {
        d.add();
        d.add();
        d.done();
        d.done();
      }
    });

  std::binary_semaphore resumed{0};
  std::atomic<int> resumes{0};
  for (int i = 0; i < kWaits; ++i) {
    // Holding an operation of our own guarantees the waiter parks;
    // releasing it races the stragglers to retire the last one.
    d.add();
    [](CoroDrain &d, std::atomic<int> &resumes,
       std::binary_semaphore &resumed) -> Detached {
      co_await d.wait_idle();
      resumes.fetch_add(1);
      resumed.release();
    }(d, resumes, resumed);
    ASSERT_EQ(resumes.load(), i);
    d.done();
    resumed.acquire();
    ASSERT_EQ(resumes.load(), i + 1);
  }

  stop.store(true, std::memory_order_relaxed);
  threads.clear();
  EXPECT_EQ(d.in_flight(), 0U);
}
