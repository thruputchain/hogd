#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <print>
#include <sys/signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <x23/async.h>
#include <x23/cleanup.h>
#include <x23/err.h>
#include <x23/event.h>
#include <x23/sockio.h>
#include <x23/threadpool.h>

// Once request_stop() has run, get_writer() must not hand out a live
// WriteBuffer: the caller can otherwise start a detached write worker
// while ~CoroPoll is tearing the poller down.
//
// The poll backend got that guard (get_reader/get_writer/get_rw all check
// stop_requested() under the state_ lock).  The epoll backend did not --
// there the checks are only against FdState::kPoisoned, and ~CoroPoll's
// reset loop sets every fd to kFree, not kPoisoned.  So on epoll a
// coroutine woken with EBADF by that same reset loop can still get a live
// writer and push bytes out of the socket after shutdown has begun.
//
// The window is not narrow: reset() schedules the EBADF callbacks on the
// thread pool, and time_to_stop() then blocks in pool_.drain() waiting for
// exactly those callbacks, so the woken coroutine is guaranteed to run
// while the poll thread is still inside time_to_stop().
//
// Sibling of poll_writer_zombie.cc, which covers the poll-only failure
// mode (a re-inserted state_ entry makes time_to_stop() never true).  On
// epoll state_ is a preallocated array, so there is no resurrection and no
// hang -- the damage is just the write that should not have happened.
// This test asserts the shared contract instead of either symptom, so it
// is meaningful on both backends.

int
main() // NOLINT(bugprone-exception-escape)
{
  signal(SIGALRM, [](int) {
    const char msg[] = "TEST TIMED OUT\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(1);
  });
  alarm(5);

  int fds[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds))
    x23::Errno{}.raise("socketpair");
  x23::set_nonblock(fds[0]);
  x23::set_nonblock(fds[1]);

  std::atomic_bool ran{false};
  std::atomic_int err{0};

  x23::ThreadPool pool(2);
  {
    x23::CoroPoll cp(pool);

    auto token = cp.read_clear(fds[0]);
    char c;
    [[maybe_unused]] int n = read(fds[0], &c, 1);
    x23assert(n == -1 && errno == EAGAIN);

    // Suspends at read_wait (the fd is not readable).  ~CoroPoll's
    // reset() wakes it with EBADF, and on its way out it tries the
    // ordinary "flush a final reply" write.
    [token, &cp, fd = fds[0], &ran, &err](this auto) -> x23::Detached {
      co_await cp.read_wait(token);
      x23::Buf reply;
      reply.push_back('x');
      err = cp.awrite(fd, std::move(reply)).raw_code();
      ran = true;
    }();

    std::println(stderr, "Destroying CoroPoll...");
    // ~CoroPoll: request_stop(), then reset(fd) wakes the coroutine
    // above, then drain() runs it -- awrite() -> get_writer().
  }

  if (!ran) {
    std::println(stderr, "INCONCLUSIVE: coroutine never reached awrite");
    return 77;
  }

  // Contract: after request_stop(), get_writer() returns nullptr, so
  // awrite() fails with EBADF and writes nothing.
  bool ok = true;
  if (err != EBADF) {
    std::println(stderr,
                 "BUG CONFIRMED: awrite() after shutdown returned {} "
                 "(expected EBADF); get_writer() handed out a live "
                 "WriteBuffer for a kFree fd",
                 err.load());
    ok = false;
  }

  char got;
  ssize_t r = read(fds[1], &got, 1);
  if (r > 0) {
    std::println(stderr,
                 "BUG CONFIRMED: {} byte(s) written to the socket after "
                 "shutdown had begun",
                 r);
    ok = false;
  }

  alarm(0);
  if (!ok)
    return 1;
  std::println(stderr, "OK -- no write after shutdown");
  return 0;
}
