
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

  int fds[2];
  if (pipe(fds))
    x23::Errno{}.raise("socketpair");
  x23::set_nonblock(fds[0]);

  x23::ThreadPool pool(2);
  {
    x23::CoroPoll cp(pool);

    auto token = cp.read_clear(fds[0]);
    char c;
    [[maybe_unused]] int n = read(fds[0], &c, 1);
    x23assert(n == -1 && errno == EAGAIN);

    // This coroutine suspends at read_wait (fd is not readable).
    // When ~CoroPoll fires reset(), the coroutine resumes and
    // calls poison() -- re-inserting a zombie entry into state_.
    [token, &cp, fd = fds[0]](this auto) -> x23::Detached {
      co_await cp.read_wait(token);
      cp.poison(fd);
    }();

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
