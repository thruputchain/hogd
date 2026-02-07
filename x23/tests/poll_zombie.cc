
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

int
main() // NOLINT(bugprone-exception-escape)
{
  signal(SIGALRM, [](int) {
    const char msg[] = "BUG CONFIRMED: ~CoroPoll() hung "
                       "(zombie fd in poll state_)\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(1);
  });
  alarm(5);

  x23::ThreadPool pool(2);
  {
    x23::CoroPoll cp(pool);

    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == -1)
      x23::Errno{}.raise("socketpair");
    x23::UniqueFd rfd(fds[0]);

    // Shrink buffers so we can fill them quickly
    int bufsz = 4096;
    setsockopt(fds[0], SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));
    setsockopt(fds[1], SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));
    x23::set_nonblock(fds[1]);

    // Fill the socket send buffer
    char junk[8192];
    memset(junk, 'x', sizeof(junk));
    while (::write(fds[1], junk, sizeof(junk)) > 0)
      ;

    // This coroutine suspends at write_wait (fd is not writable).
    // When ~CoroPoll fires reset(), the coroutine resumes and
    // calls poison() -- re-inserting a zombie entry into state_.
    [](x23::CoroPoll &cp, int fd) -> x23::Detached {
      co_await cp.write_wait(cp.write_clear(fd));
      cp.poison(fd);
    }(cp, fds[1]);

    // Let the coroutine reach write_wait and suspend
    usleep(100'000);

    std::println(stderr, "Destroying CoroPoll (will hang if bug present)...");
    // ~CoroPoll runs here:
    //   reset(fds[1]) erases state_ entry, schedules coroutine
    //   drain() resumes coroutine -> poison() creates zombie
    //   time_to_stop() sees !state_->empty() -> loops forever
  }

  alarm(0);
  std::println(stderr, "OK -- no hang (bug is fixed or epoll platform)");
  return 0;
}
