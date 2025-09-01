# Detects whether the compiler has this problem:
# https://gcc.gnu.org/bugzilla/show_bug.cgi?id=121643

include(CheckCXXSourceCompiles)

check_cxx_source_compiles([=[
#include <coroutine>
#include <exception>
#include <source_location>

struct Detached {
  struct promise_type {
    Detached get_return_object() const noexcept { return {}; }
    std::suspend_never initial_suspend() const noexcept { return {}; }
    void return_void() const noexcept {}
    void unhandled_exception() const noexcept { std::terminate(); }
    std::suspend_never final_suspend() const noexcept { return {}; }
  };
};

struct Awaiter {
  bool await_ready() noexcept { return true; }
  void
  await_suspend(std::coroutine_handle<> h,
                std::source_location sloc = std::source_location::current())
    noexcept
  {}
  void await_resume() noexcept {}
};

Detached
coroutine()
{
  co_await Awaiter{};
}
]=] X23_CORO_SLOCK_OK)
