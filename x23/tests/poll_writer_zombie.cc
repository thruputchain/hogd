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

// Sibling of poll_zombie.cc.  There, the zombie state_ entry came from
// poison(), which now checks stop_requested() under the state_ lock.
// get_reader(), get_writer() and get_rw() have no such check, so
// awrite()/dwrite() -- which reach get_writer() with no stop check of
// their own -- can still re-insert an entry after ~CoroPoll's reset loop
// has run.
//
// This is not a narrow race.  reset() schedules the EBADF callbacks on the
// thread pool, and time_to_stop() then blocks in pool_.drain() waiting for
// exactly those callbacks, so a coroutine woken by shutdown is guaranteed
// to run while the poll thread is still inside time_to_stop().
//
// poll backend only: the epoll time_to_stop() keys on numcoro_ rather than
// on state_ being empty, so this exits cleanly there.

int
main() // NOLINT(bugprone-exception-escape)
{
  signal(SIGALRM, [](int) {
    const char msg[] = "BUG CONFIRMED: ~CoroPoll() hung "
                       "(zombie fd re-inserted by get_writer)\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(1);
  });
  alarm(5);

  // socketpair, not pipe:  the coroutine below does a real write on the
  // same fd it was reading, so the write must genuinely succeed.  This is
  // the ordinary "flush a final reply after the read fails" path.
  int fds[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds))
    x23::Errno{}.raise("socketpair");
  x23::set_nonblock(fds[0]);

  x23::ThreadPool pool(2);
  {
    x23::CoroPoll cp(pool);

    auto token = cp.read_clear(fds[0]);
    char c;
    [[maybe_unused]] int n = read(fds[0], &c, 1);
    x23assert(n == -1 && errno == EAGAIN);

    // This coroutine suspends at read_wait (fd is not readable).  When
    // ~CoroPoll fires reset(), the coroutine resumes with EBADF and writes
    // on its way out -- dwrite() -> get_writer() -> (*state_)[fd], which
    // puts the fd back into state_ after the reset loop is done with it.
    [token, &cp, fd = fds[0]](this auto) -> x23::Detached {
      co_await cp.read_wait(token);
      x23::Buf reply;
      reply.push_back('x');
      cp.dwrite(fd, std::move(reply));
    }();

    std::println(stderr, "Destroying CoroPoll (will hang if bug present)...");
    // ~CoroPoll runs here:
    //   reset(fds[0]) erases the state_ entry, schedules the coroutine
    //   drain() resumes the coroutine -> dwrite() re-creates the entry
    //   time_to_stop() sees !state_->empty() -> loops forever
  }

  alarm(0);
  std::println(stderr, "OK -- no hang (bug is fixed or epoll platform)");
  return 0;
}
