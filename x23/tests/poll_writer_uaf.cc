#include <csignal>
#include <cstdio>
#include <print>
#include <sys/socket.h>
#include <unistd.h>

#include <x23/async.h>
#include <x23/err.h>
#include <x23/event.h>
#include <x23/sockio.h>
#include <x23/threadpool.h>
#include <x23/vec.h>

// Sibling of poll_writer_zombie.cc, for the other end of the writer
// shutdown path: not a zombie fd entry, but a use-after-free of the
// WriteBuffer that a parked writer coroutine is servicing.
//
// detail::WriteBuffer::Service holds a std::unique_lock on
// wb.service_lock_, a TestLock *inside the WriteBuffer*.  The
// Service::worker coroutine keeps the buffer alive with an ordinary
// RefPtr local, but a coroutine's parameter copies outlive the body's
// locals: the body's RefPtr goes away at co_return, and only then does
// the frame's `Service self` parameter get destroyed and unlock.  If
// that RefPtr held the last reference, ~Service unlocks a TestLock in
// freed memory.
//
// This is the ordinary teardown path, not a narrow race.  reset()
// drops the fd's writer reference (buf_clear) and only afterwards
// wakes the parked writer with EBADF (setmode -> docbs), so the
// coroutine is guaranteed to resume with the fd table's reference
// already gone -- its own is the last one.
//
// With the bug present this aborts in TestLock::unlock(), because
// freeing the WriteBuffer overwrites service_lock_ with allocator
// bookkeeping, so the unlock sees neither kLocked nor
// kLockedWantNotify.  Under ASAN it is reported as a
// heap-use-after-free instead, with the same stack.

int
main() // NOLINT(bugprone-exception-escape)
{
  signal(SIGALRM, [](int) {
    const char msg[] = "TEST INCONCLUSIVE: writer never finished\n";
    ssize_t n = write(STDERR_FILENO, msg, sizeof(msg) - 1);
    (void)n;
    _exit(1);
  });
  alarm(10);

  int fds[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds))
    x23::Errno{}.raise("socketpair");
  x23::set_nonblock(fds[0]);

  // Shrink both ends so that the write below cannot possibly drain
  // into the socket, and nothing ever reads fds[1].
  int bufsz = 4096;
  if (setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz)) ||
      setsockopt(fds[1], SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz)))
    x23::Errno{}.raise("setsockopt");

  x23::ThreadPool pool(2);
  {
    x23::CoroPoll cp(pool);

    // Far more than the socket buffers can hold, so writev returns a
    // short count: flush() spawns Service::worker, which parks in
    // write_wait() still holding wb->service_lock_.
    x23::Buf big;
    big.resize(4 << 20, 'x');
    if (x23::Errno err = cp.awrite(fds[0], std::move(big)))
      x23::panic("awrite: {}", err);

    std::println(stderr, "writer parked; resetting fd "
                         "(aborts in TestLock::unlock if bug present)...");

    // Drops the fd's writer reference, then wakes the parked writer
    // with EBADF.  The worker co_returns holding the service lock.
    cp.reset(fds[0]);

    // Run the resumed coroutine to completion here rather than in
    // ~CoroPoll, so a failure stack is about the writer and not about
    // shutdown.
    pool.drain();
  }

  alarm(0);
  close(fds[0]);
  close(fds[1]);
  std::println(stderr, "OK -- writer unlocked before releasing its buffer");
  return 0;
}
