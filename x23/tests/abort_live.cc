// Test that abort() correctly cancels a live pending I/O callback.
// On the epoll backend, this exercises the numcoro_ decrement in abort().
// Without the fix, ~CoroPoll() hangs because numcoro_ never reaches 0.

#include "x23/debug.h"
#include <atomic>
#include <cerrno>
#include <csignal>
#include <print>
#include <unistd.h>

#include <x23/async.h>
#include <x23/cleanup.h>
#include <x23/err.h>
#include <x23/event.h>
#include <x23/sockio.h>
#include <x23/threadpool.h>

int
main() // NOLINT(bugprone-exception-escape)
{
  signal(SIGALRM, [](int) {
    const char msg[] = "BUG CONFIRMED: ~CoroPoll() hung "
                       "(numcoro_ not decremented by abort())\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(1);
  });
  alarm(5);

  int fds[2];
  if (pipe(fds))
    x23::Errno{}.raise("pipe");
  x23::set_nonblock(fds[0]);

  x23::ThreadPool pool(2);
  {
    x23::CoroPoll cp(pool);
    std::atomic<bool> done{false};
    std::atomic<int> result_err{-1};

    // Do setup before the coroutine so captures are simple.
    auto token = cp.read_clear(fds[0]);
    char c;
    [[maybe_unused]] int n = read(fds[0], &c, 1);
    x23assert(n == -1 && errno == EAGAIN);

    // The Detached coroutine runs synchronously until co_await.
    // By the time the invocation returns, setcb() has been called
    // (registering the callback and, on epoll, incrementing numcoro_).
    [token, &cp, &done, &result_err](this auto) -> x23::Detached {
      auto err = co_await cp.read_wait(token);
      result_err.store(err.raw_code(), std::memory_order_release);
      done.store(true, std::memory_order_release);
    }();

    // Verify the coroutine actually suspended (callback is in the slot).
    // If it didn't suspend, setcb fired the callback synchronously —
    // meaning epoll reported readiness we didn't expect.
    if (done.load(std::memory_order_acquire)) {
      std::println(stderr,
                   "DIAGNOSTIC: coroutine did NOT suspend "
                   "(setcb fired synchronously, err={})",
                   result_err.load(std::memory_order_acquire));
      std::println(stderr, "abort() path was never exercised");
      return 2;
    }

    // Callback is live in the slot. abort() must exchange it out
    // AND decrement numcoro_ (epoll) so time_to_stop() can return true.
    cp.abort(fds[0], false);

    // Wait for the thread pool to resume the coroutine
    while (!done.load(std::memory_order_acquire))
      usleep(1000);

    int err = result_err.load(std::memory_order_acquire);
    std::println(stderr, "coroutine resumed with err={} (ETIMEDOUT={})", err,
                 ETIMEDOUT);
    if (err != ETIMEDOUT)
      std::println(stderr, "DIAGNOSTIC: callback was NOT fired by abort() "
                           "— something else resumed it first");

    cp.reset(fds[0]);

    std::println(stderr, "Destroying CoroPoll (will hang if numcoro_ bug)...");
    // ~CoroPoll: on epoll, time_to_stop() checks numcoro_ == 0.
    // Without the --numcoro_ in abort(), it stays at 1 → hang → SIGALRM.
  }

  alarm(0);
  ::close(fds[0]);
  ::close(fds[1]);
  std::println(stderr, "OK -- no hang");
  return 0;
}
