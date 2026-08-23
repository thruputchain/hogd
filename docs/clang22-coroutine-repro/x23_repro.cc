#include <x23/async.h>
#include <cstdio>
#include <thread>
#include <utility>
using namespace x23;

using Result = std::pair<int, long>;
using MA = MaybeAsync<Result>;

static Async<int> leaf(Async<int> &pending)
{
  int v = co_await pending;
  co_return v;
}

// Coroutine declared MaybeAsync<T>: promise comes from Async<T> via
// x23's coroutine_traits inheritance, so get_return_object()'s type
// (Async<T>) differs from the declared return type (MaybeAsync<T>).
static MA inner(Async<int> &pending)
{
  int v = co_await leaf(pending);
  co_return Result{v + 1, v + 2};
}

static Async<int> outer(Async<int> pending)
{
  MA ma = inner(pending);
  Result r = co_await ma;    // ~MA here destroys inner's frame on the
  co_return r.first + (int)r.second;   // resuming (completer) thread
}

// Fill the completer thread's stack with non-null garbage so the
// destroy clone's uninitialized cleanup slot holds a poison "handle".
__attribute__((noinline)) static void dirty_stack()
{
  volatile unsigned long long buf[2048];
  for (auto &x : buf)
    x = 0x5151515151515151ULL;
}

int main()
{
  for (int i = 0; i < 50; i++) {
    auto [async_in, cb] = make_async<int>();
    Async<int> out = outer(std::move(async_in));
    std::thread completer([cb] {
      dirty_stack();
      cb(41);
    });
    int result = out();
    completer.join();
    if (result != 85) { std::printf("iter %d: wrong %d\n", i, result); return 1; }
  }
  std::printf("ok\n");
  return 0;
}
