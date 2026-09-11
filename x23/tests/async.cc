#include <concepts>
#include <coroutine>
#include <exception>
#include <expected>
#include <memory>
#include <semaphore>
#include <source_location>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/threadpool.h"

using namespace x23;

namespace {

Suspended
record(std::vector<int> &events, int value)
{
  events.push_back(value);
  co_return;
}

CoResume
callback_value(int value, Callback<int> cb)
{
  co_return value;
}

CoResume
callback_void(bool &ran, Callback<void> cb)
{
  ran = true;
  co_return;
}

CoResume
callback_error(Callback<void> cb)
{
  throw std::runtime_error("callback error");
  co_return;
}

CoResume
expected_callback(std::expected<int, int> input, bool &reached,
                  Callback<std::expected<int, int>> cb)
{
  int value = co_await input;
  reached = true;
  co_return value + 1;
}

Suspended
located_suspended(bool &ran, source_location location)
{
  ran = true;
  co_return;
}

Detached
located_detached(bool &ran, source_location location)
{
  ran = true;
  co_return;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
void
legacy_callback(Callback<int> cb)
{
  co_return 41;
}
#pragma GCC diagnostic pop

struct AdlAwaitable {
  int value_;
};

struct ReadyValue {
  int value_;
  bool await_ready() const noexcept { return true; }
  void await_suspend(std::coroutine_handle<>) const noexcept {}
  int await_resume() const noexcept { return value_; }
};

struct ManualAwaiter {
  std::coroutine_handle<> &pending_;
  bool await_ready() const noexcept { return false; }
  void await_suspend(std::coroutine_handle<> h) noexcept { pending_ = h; }
  int await_resume() const noexcept { return 21; }
};

ReadyValue
operator co_await(AdlAwaitable a) noexcept
{
  return {a.value_};
}

// The three legal await_suspend return types have different ownership
// rules when final suspension is emulated by an early return.
template<typename R> struct FinalAwaiter {
  bool ready_;
  bool suspend_;
  int &calls_;
  std::coroutine_handle<> next_ = std::noop_coroutine();

  bool await_ready() const noexcept { return ready_; }
  R await_suspend(std::coroutine_handle<>) const noexcept
  {
    ++calls_;
    if constexpr (std::same_as<R, bool>)
      return suspend_;
    else if constexpr (!std::is_void_v<R>)
      return next_;
  }
  void await_resume() const noexcept {}
};

template<typename A> struct FinalTask {
  struct promise_type {
    A final_;
    int &destroyed_;
    promise_type(A a, int &destroyed) : final_(a), destroyed_(destroyed) {}
    ~promise_type() { ++destroyed_; }
    FinalTask get_return_object()
    {
      return {std::coroutine_handle<promise_type>::from_promise(*this)};
    }
    std::suspend_always initial_suspend() noexcept { return {}; }
    A final_suspend() noexcept { return final_; }
    void return_void() noexcept {}
    void unhandled_exception() { std::terminate(); }
  };
  std::coroutine_handle<promise_type> h_;
};

template<typename A>
FinalTask<A>
final_task(A a, int &destroyed)
{
  co_return;
}

} // namespace

TEST(CoResume, MoveAndAssignmentResumeExactlyOnce)
{
  std::vector<int> events;
  {
    CoResume empty;
    CoResume first{record(events, 1)};
    CoResume moved{std::move(first)};
    EXPECT_TRUE(events.empty());
    moved = CoResume{record(events, 2)};
    EXPECT_EQ(events, (std::vector{1}));
    empty = std::move(moved);
    EXPECT_EQ(events, (std::vector{1}));
  }
  EXPECT_EQ(events, (std::vector{1, 2}));
}

TEST(CallbackPromise, ValueAndVoidStartOnDestruction)
{
  int value = 0;
  {
    auto start = callback_value(17, Callback{[&](int v) { value = v; }});
    EXPECT_EQ(value, 0);
  }
  EXPECT_EQ(value, 17);
  bool ran = false, called = false;
  {
    auto start = callback_void(ran, Callback{[&] { called = true; }});
    EXPECT_FALSE(ran);
    EXPECT_FALSE(called);
  }
  EXPECT_TRUE(ran);
  EXPECT_TRUE(called);
  legacy_callback(Callback{[&](int v) { value = v; }});
  EXPECT_EQ(value, 41);
}

TEST(GetCoro, ReturnsTheRunningCoroutineHandle)
{
  auto task = [] -> Async<void> {
    auto typed = co_await GetCoro<Async<void>::promise_type>{};
    auto untyped = co_await GetCoro{};
    EXPECT_EQ(typed.address(), untyped.address());
    EXPECT_FALSE(typed.done());
  }();
  task();
}

TEST(CoroutinePromise, ExplicitSourceLocationKeepsDeferredStart)
{
  bool ran = false;
  auto suspended = located_suspended(ran, source_location::current());
  EXPECT_FALSE(ran);
  suspended.resume();
  EXPECT_TRUE(ran);
  ran = false;
  {
    auto detached = located_detached(ran, source_location::current());
    EXPECT_FALSE(ran);
  }
  EXPECT_TRUE(ran);
}

TEST(Awaitable, CompletionBetweenReadyAndSuspendDoesNotSuspend)
{
  Awaitable<int> result;
  CoroCB cb{result};
  EXPECT_TRUE(bool(cb));
  EXPECT_FALSE(bool(CoroCB<int>{}));
  EXPECT_FALSE(bool(CoroCB<int>{nullptr}));
  EXPECT_FALSE(result.await_ready());
  cb(29);
  EXPECT_FALSE(result.await_suspend(std::noop_coroutine()));
  EXPECT_EQ(result.await_resume(), 29);
}

TEST(Awaitable, ExceptionBeforeAndAfterSuspension)
{
  for (bool early : {false, true}) {
    Awaitable<int> result;
    CoroCB cb{result};
    auto error = std::make_exception_ptr(std::runtime_error("callback error"));
    if (early)
      cb.raise(error);
    auto task = [&](this auto) -> Async<int> { co_return co_await result; }();
    if (!early)
      cb.raise(error);
    EXPECT_THROW(task(), std::runtime_error);
  }
}

TEST(CoroCB, DeferredResumptionAndEarlyResult)
{
  Awaitable<std::unique_ptr<int>> result;
  CoroCB cb{result};
  auto early = cb.set_result_without_resuming(std::make_unique<int>(11));
  EXPECT_FALSE(bool(early));
  EXPECT_EQ(*result.await_resume(), 11);

  Awaitable<void> done;
  CoroCB finish{done};
  bool resumed = false;
  auto task = [&](this auto) -> Async<void> {
    co_await done;
    resumed = true;
  }();
  auto h = finish.set_result_without_resuming();
  EXPECT_TRUE(bool(h));
  EXPECT_FALSE(resumed);
  h();
  EXPECT_TRUE(resumed);
  task();
}

TEST(Async, MoveOnlyResultsAndFrameCleanup)
{
  int destroyed = 0;
  auto task = [&](this auto) -> Async<std::unique_ptr<int>> {
    Defer cleanup{[&] { ++destroyed; }};
    co_return std::make_unique<int>(42);
  }();
  auto moved = std::move(task);
  EXPECT_EQ(*moved(), 42);
  EXPECT_EQ(destroyed, 1);
  auto failed = [] -> Async<int> {
    throw std::logic_error("async error");
    co_return 0;
  }();
  EXPECT_THROW(failed(), std::logic_error);
}

TEST(Async, BlockingWaitAndCrossThreadCompletion)
{
  ThreadPool pool(16);
  for (int i = 0; i < 256; ++i) {
    auto task = [&](this auto) -> Async<int> {
      co_await pool.bg();
      co_return i;
    }();
    EXPECT_EQ(task(), i);
  }
  pool.drain();
}

TEST(Async, CallbackCompletionRacesBlockingWait)
{
  constexpr int kIterations = 10000;
  std::binary_semaphore start{0};
  CoroCB<int> pending;
  std::jthread completer([&] {
    for (int i = 0; i < kIterations; ++i) {
      start.acquire();
      pending(i);
    }
  });
  for (int i = 0; i < kIterations; ++i) {
    auto [task, cb] = make_async<int>();
    pending = cb;
    // Race completion with the readiness check and allocation of the
    // blocking wait's semaphore-signalling coroutine.
    start.release();
    EXPECT_EQ(task(), i);
  }
}

TEST(Async, TypedAndConvertedCallbacks)
{
  int result = 0;
  async_return(3)([&](int v) { result = v; });
  EXPECT_EQ(result, 3);
  Awaitable<int> done;
  async_return(5)(CoroCB{done});
  EXPECT_EQ(done.await_resume(), 5);
  auto [task, cb] = make_async<void>();
  bool called = false;
  task(Callback{[&] { called = true; }});
  EXPECT_FALSE(called);
  cb();
  EXPECT_TRUE(called);
}

TEST(MaybeAsync, ImmediateValueAndVoidCalls)
{
  MaybeAsync value{std::make_unique<int>(9)};
  EXPECT_TRUE(value.has_value());
  EXPECT_TRUE(value.await_ready());
  EXPECT_EQ(*value(), 9);
  MaybeAsync<void> nothing;
  EXPECT_TRUE(nothing.has_value());
  nothing();
  nothing.await_resume();
  int result = 0;
  MaybeAsync{4}([&](int v) { result = v; });
  EXPECT_EQ(result, 4);
  MaybeAsync{6}(Callback{[&](int v) { result = v; }});
  EXPECT_EQ(result, 6);
  bool called = false;
  MaybeAsync<void>{}([&] { called = true; });
  EXPECT_TRUE(called);
  called = false;
  MaybeAsync<void>{}(Callback{[&] { called = true; }});
  EXPECT_TRUE(called);
}

TEST(MaybeAsync, AsynchronousValueAndVoidCalls)
{
  MaybeAsync value{async_return(7)};
  EXPECT_FALSE(value.has_value());
  EXPECT_EQ(value(), 7);
  int result = 0;
  MaybeAsync{async_return(8)}([&](int v) { result = v; });
  EXPECT_EQ(result, 8);
  MaybeAsync{async_return(9)}(Callback{[&](int v) { result = v; }});
  EXPECT_EQ(result, 9);

  auto [task, cb] = make_async<void>();
  MaybeAsync pending{std::move(task)};
  bool called = false;
  pending(Callback{[&] { called = true; }});
  EXPECT_FALSE(called);
  cb();
  EXPECT_TRUE(called);
}

TEST(MaybeAsync, AwaitReadyAndPendingAlternatives)
{
  auto [task, cb] = make_async<int>();
  auto caller = [&](this auto) -> Async<int> {
    EXPECT_EQ(co_await MaybeAsync{12}, 12);
    co_await MaybeAsync<void>{};
    EXPECT_EQ(co_await MaybeAsync{async_return(13)}, 13);
    co_return co_await MaybeAsync{std::move(task)};
  }();
  cb(14);
  EXPECT_EQ(caller(), 14);
  auto [nothing, finish] = make_async<void>();
  auto waiter = [&](this auto) -> Async<void> {
    co_await MaybeAsync{std::move(nothing)};
  }();
  finish();
  waiter();
}

TEST(MapAwait, DirectAdlAndMemberAwaiters)
{
  auto task = [] -> Async<int> {
    int a = co_await MapAwait{[](int v) { return v * 2; }, ReadyValue{3}};
    int b = co_await MapAwait{[](int v) { return v + 1; }, AdlAwaitable{4}};
    int c = co_await MapAwait{[] { return 7; }, std::suspend_never{}};
    // GCC 15 tries to copy Async here if the MapAwait is a temporary.
    auto member = MapAwait{[](int v) { return v; }, async_return(8)};
    int d = co_await member;
    co_return a + b + c + d;
  }();
  EXPECT_EQ(task(), 26);
}

TEST(MapAwait, SuspendsAndPropagatesExceptions)
{
  auto [task, cb] = make_async<int>();
  bool mapped = false;
  auto caller = [&](this auto) -> Async<int> {
    co_return co_await MapAwait{[&](int v) {
                                  mapped = true;
                                  return v * 2;
                                },
                                std::move(task)};
  }();
  EXPECT_FALSE(mapped);
  cb(21);
  EXPECT_EQ(caller(), 42);
  EXPECT_TRUE(mapped);
  auto failed = [] -> Async<int> {
    co_return co_await MapAwait{[](int) -> int {
                                  throw std::runtime_error("mapping error");
                                },
                                ReadyValue{0}};
  }();
  EXPECT_THROW(failed(), std::runtime_error);
}

TEST(MapAwait, ForwardsSuspensionWithoutSourceLocation)
{
  std::coroutine_handle<> pending;
  auto task = [&](this auto) -> Async<int> {
    co_return co_await MapAwait{[](int v) { return v * 2; },
                                ManualAwaiter{pending}};
  }();
  ASSERT_TRUE(bool(pending));
  pending.resume();
  EXPECT_EQ(task(), 42);
}

TEST(ExpectedAwait, ValuesErrorsAndCleanup)
{
  for (bool success : {false, true}) {
    int destroyed = 0;
    bool reached = false;
    {
      auto task = [&](this auto) -> Async<std::expected<int, std::string>> {
        Defer cleanup{[&] { ++destroyed; }};
        const std::expected<int, std::string> first{2};
        int a = co_await first;
        auto second = success ? std::expected<int, std::string>{3}
                              : std::unexpected(std::string("failed"));
        int b = co_await std::move(second);
        reached = true;
        co_return a + b;
      }();
      auto result = task();
      EXPECT_EQ(result.has_value(), success);
      EXPECT_EQ(reached, success);
      if (success)
        EXPECT_EQ(*result, 5);
      else
        EXPECT_EQ(result.error(), "failed");
      // Early return destroys locals when the owning Async is destroyed.
      if (success) {
        EXPECT_EQ(destroyed, 1);
      }
    }
    EXPECT_EQ(destroyed, 1);
  }
}

TEST(ExpectedAwait, VoidAndMoveOnlyValues)
{
  for (bool success : {false, true}) {
    bool reached = false;
    auto task = [&](this auto) -> Async<std::expected<int, int>> {
      std::expected<void, int> check;
      if (!success)
        check = std::unexpected(19);
      co_await check;
      auto value = co_await std::expected<std::unique_ptr<int>, int>{
          std::make_unique<int>(37)};
      reached = true;
      co_return *value;
    }();
    auto result = task();
    EXPECT_EQ(reached, success);
    if (success)
      EXPECT_EQ(result.value(), 37);
    else
      EXPECT_EQ(result.error(), 19);
  }
}

TEST(ExpectedAwait, CallbackEarlyReturnAndSuccess)
{
  for (bool success : {false, true}) {
    bool reached = false, called = false;
    auto input = success ? std::expected<int, int>{5} : std::unexpected(17);
    expected_callback(input, reached,
                      Callback{[&](std::expected<int, int> result) {
                        called = true;
                        EXPECT_EQ(result.has_value(), success);
                        if (success)
                          EXPECT_EQ(*result, 6);
                        else
                          EXPECT_EQ(result.error(), 17);
                      }});
    EXPECT_TRUE(called);
    EXPECT_EQ(reached, success);
  }
}

TEST(FinalSuspend, VoidAwaiterDestroysOnlyWhenReady)
{
  for (bool ready : {false, true}) {
    int calls = 0, destroyed = 0;
    auto task = final_task(FinalAwaiter<void>{ready, true, calls}, destroyed);
    detail::emulate_final_suspend(task.h_);
    EXPECT_EQ(calls, ready ? 0 : 1);
    EXPECT_EQ(destroyed, ready ? 1 : 0);
    if (!ready)
      task.h_.destroy();
    EXPECT_EQ(destroyed, 1);
  }
}

TEST(FinalSuspend, BooleanAwaiterControlsFrameLifetime)
{
  for (bool ready : {false, true})
    for (bool suspend : {false, true}) {
      int calls = 0, destroyed = 0;
      auto task =
          final_task(FinalAwaiter<bool>{ready, suspend, calls}, destroyed);
      detail::emulate_final_suspend(task.h_);
      EXPECT_EQ(calls, ready ? 0 : 1);
      EXPECT_EQ(destroyed, ready || !suspend ? 1 : 0);
      if (!destroyed)
        task.h_.destroy();
      EXPECT_EQ(destroyed, 1);
    }
}

TEST(FinalSuspend, HandleAwaiterReturnsContinuation)
{
  for (bool ready : {false, true}) {
    int calls = 0, destroyed = 0;
    std::vector<int> events;
    auto next = record(events, 1);
    auto task = final_task(FinalAwaiter<std::coroutine_handle<>>{ready, true,
                                                                 calls, next},
                           destroyed);
    auto h = detail::emulate_final_suspend(task.h_);
    EXPECT_EQ(calls, ready ? 0 : 1);
    EXPECT_EQ(destroyed, ready ? 1 : 0);
    h.resume();
    if (ready) {
      EXPECT_TRUE(events.empty());
      next.destroy();
    }
    else {
      EXPECT_EQ(events, (std::vector{1}));
      task.h_.destroy();
    }
    EXPECT_EQ(destroyed, 1);
  }
}

TEST(AsyncDeathTest, FireAndForgetExceptionsAreFatal)
{
  EXPECT_DEATH(([] -> Detached {
                 throw std::runtime_error("detached error");
                 co_return;
               }()),
               "detached error");
  EXPECT_DEATH(([] -> Suspended {
                 throw std::runtime_error("suspended error");
                 co_return;
               }()
                          .resume()),
               "suspended error");
  EXPECT_DEATH(callback_error(Callback{[] {}}), "callback error");
}
