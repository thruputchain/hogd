#include <csignal>
#include <cstddef>
#include <cstdint>
#include <print>

#include <sys/socket.h>
#include <unistd.h>

#include <x23/err.h>
#include <x23/event.h>
#include <x23/sockio.h>
#include <x23/threadpool.h>

// A detached WriteBuffer::Service::worker() has to release
// wb->service_lock_ *before* it drops its reference to the WriteBuffer,
// because the lock lives inside the WriteBuffer.  worker() used to hold
// the reference in a body local, which gets that order exactly backwards:
// a coroutine's body locals are destroyed when the body finishes, while
// the frame -- including the Service parameter that owns the lock -- is
// destroyed afterwards.  So ~unique_lock unlocked freed memory.
//
// The window is not hypothetical.  ~CoroPoll resets every file
// descriptor, which drops the BufHolder's reference and wakes the
// suspended worker with EBADF; the worker's own reference is then the
// last one, and it co_returns with the lock still held.  This test
// reproduces that sequence with a socketpair, no TCP and no timing
// assumptions: the write below cannot drain, so the worker is always
// suspended in write_wait() holding the lock when the CoroPoll goes
// away.
//
// How the bug shows up, given the write-after-free is unconditional:
//
//   * asan (the sanitizer schedule runs the whole suite under clang and
//     gcc asan, debug and release): heap-use-after-free on the unlock,
//     which is the cleanest report of the four.
//   * debug: TestLock::unlock() panics with "x23assert failure: old ==
//     kLocked", because the freed WriteBuffer no longer reads back as
//     locked.  MALLOC_PERTURB_/MallocScribble (set on the ctest, see
//     x23/AGENTS.md) fills the block for us where the allocator does not
//     already scribble on it.  This is how the asyncio stress schedule
//     caught it in rpcthroughput:
//
//       test_rpcthroughput:PANIC: x23assert failure: old == kLocked
//         function void x23::TestLock::unlock()
//         x23::detail::WriteBuffer::Service::~Service()
//
//   * plain release: not observable here -- x23assert is compiled out,
//     and the store is just a byte written into a free chunk.

int
main() // NOLINT(bugprone-exception-escape)
{
  signal(SIGALRM, [](int) {
    const char msg[] = "TIMEOUT: ~CoroPoll did not finish\n";
    [[maybe_unused]] ssize_t n = write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(1);
  });
  alarm(10);

  // socketpair with small buffers, and nobody ever drains the peer, so
  // the write leaves work behind: WriteBuffer::flush() spawns the
  // detached worker(), which suspends in write_wait() holding
  // wb->service_lock_.
  int fds[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds))
    x23::Errno{}.raise("socketpair");
  x23::set_nonblock(fds[0]);
  int bufsz = 4096;
  if (setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz)) ||
      setsockopt(fds[1], SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz)))
    x23::Errno{}.raise("setsockopt");

  // Everything below only means something if the write cannot finish
  // inside flush(), so check that the socket really cannot swallow the
  // whole payload rather than trust the setsockopt calls above.
  constexpr size_t kPayload = 4 << 20;
  int sndbuf = 0;
  int rcvbuf = 0;
  socklen_t optlen = sizeof(int);
  if (getsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, &optlen) ||
      getsockopt(fds[1], SOL_SOCKET, SO_RCVBUF, &rcvbuf, &optlen))
    x23::Errno{}.raise("getsockopt");
  if (size_t(sndbuf) + size_t(rcvbuf) >= kPayload) {
    std::println(stderr,
                 "SETUP FAILED: socket buffers ({} + {}) can take the whole "
                 "{}-byte write, so no worker would suspend",
                 sndbuf, rcvbuf, kPayload);
    return 1;
  }

  x23::ThreadPool pool(2);
  {
    x23::CoroPoll cp(pool);

    x23::Buf data(kPayload, uint8_t{'x'});
    if (x23::Errno err = cp.awrite(fds[0], std::move(data))) {
      // No worker to leave holding the lock, so the run proves nothing.
      std::println(stderr, "SETUP FAILED: awrite: {}", err);
      return 1;
    }

    std::println(stderr, "destroying CoroPoll with a write still pending...");
    // ~CoroPoll runs here:
    //   reset(fds[0]) drops the BufHolder's WriteBuffer reference
    //   drain() resumes worker() with EBADF -> co_return
    //   the worker held the last reference, so the WriteBuffer is freed
    //   -- and must not be unlocked afterwards
  }

  alarm(0);
  close(fds[0]);
  close(fds[1]);
  std::println(stderr, "OK -- WriteBuffer was not unlocked after being freed");
  return 0;
}
