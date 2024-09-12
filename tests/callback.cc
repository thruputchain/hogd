#include <cassert>
#include <print>
#include <source_location>
#include <utility>

#include <x23/async.h>

using namespace x23;

std::function<void()> step;

void
update_step(CoroCB<void> r)
{
  assert(!step);
  step = [r] {
    auto _keep_lambda = std::exchange(step, nullptr);
    r();
  };
}

Async<void>
suspend(std::source_location sl = std::source_location::current())
{
  std::println("suspend at {}:{}", sl.file_name(), sl.line());
  Awaitable<void> a;
  update_step(a);
  co_await a;
  std::println("resume at {}:{}", sl.file_name(), sl.line());
}

void
loop(std::source_location sl = std::source_location::current())
{
  while (step) {
    std::println("take step in loop at {}:{}", sl.function_name(), sl.line());
    step();
  }
}

Async<int>
add5(int n)
{
  co_return n + 5;
}

Async<int>
add(int n1, int n2)
{
  std::println("add {} + {}", n1, n2);
  co_await suspend();
  std::println("add {} + {} (resumed)", n1, n2);
  co_return n1 + n2;
}

Async<int>
coro_function(int n1, int n2)
{
  n1 = co_await add5(n1);
  co_await suspend();

  n2 = co_await add5(n2);
  co_return co_await add(n1, n2);
}

Async<void>
dumb_test()
{
  co_return;
}

int
main()
{
  setlinebuf(stdout);

  coro_function(1, 2)([](int n) {
    std::println("***** {}", n);
    assert(n == 13);
  });

  loop();

  add(31, 29)([](int n) {
    std::println("***** {}", n);
    assert(n == 60);
  });

  loop();

  return 0;
}
