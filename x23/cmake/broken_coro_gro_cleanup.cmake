# Detects whether the compiler has this problem:
# https://github.com/llvm/llvm-project/issues/193412
#
# A coroutine whose promise's get_return_object() type differs from the
# coroutine's declared return type may incorrectly retain the GRO
# temporary's cleanup in the generated coroutine .destroy function.

include(CheckCXXSourceRuns)

# The LLVM regression test exercises this at -O2.  Make the feature test
# independent of the optimization level used for the x23 build.
set(_CMAKE_REQUIRED_FLAGS ${CMAKE_REQUIRED_FLAGS})
set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -O2")

check_cxx_source_runs([=[
#include <coroutine>
#include <exception>
#include <utility>

static int destroys;

struct Task {
  struct promise_type {
    Task
    get_return_object() noexcept
    {
      return Task{
        std::coroutine_handle<promise_type>::from_promise(*this)
      };
    }

    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }
    void return_void() noexcept {}
    void unhandled_exception() noexcept { std::terminate(); }
  };

  std::coroutine_handle<promise_type> h;

  explicit Task(std::coroutine_handle<promise_type> h_) noexcept
    : h(h_)
  {}

  Task(Task &&other) noexcept
    : h(std::exchange(other.h, {}))
  {}

  Task(const Task &) = delete;

  ~Task()
  {
    ++destroys;
  }
};

struct Wrapper {
  using promise_type = Task::promise_type;

  Task task;

  Wrapper(Task &&task_) noexcept
    : task(std::move(task_))
  {}
};

Wrapper
coroutine()
{
  co_return;
}

int
main()
{
  Wrapper w = coroutine();

  // get_return_object() produced a Task which was moved into Wrapper.
  // Its moved-from temporary should have been destroyed exactly once
  // in the coroutine ramp.
  if (destroys != 1)
    return 1;

  // With LLVM bug #193412, the cleanup for that already-destroyed GRO
  // temporary is also emitted into coroutine.destroy, so this increments
  // destroys a second time.
  w.task.h.destroy();

  // The destructor of w.task runs after evaluation of this return
  // expression, so it cannot affect the result of the test.
  return destroys == 1 ? 0 : 2;
}
]=] X23_CORO_GRO_CLEANUP_OK)

set(CMAKE_REQUIRED_FLAGS ${_CMAKE_REQUIRED_FLAGS})

if(X23_CORO_GRO_CLEANUP_OK)
  set(HAVE_BROKEN_CORO_GRO_CLEANUP 0 CACHE INTERNAL
    "compiler correctly handles coroutine get_return_object cleanup")
else()
  set(HAVE_BROKEN_CORO_GRO_CLEANUP 1 CACHE INTERNAL
    "compiler incorrectly emits get_return_object cleanup in coroutine destroy")
endif()
