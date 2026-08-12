#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>
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
// last one, and it co_returns with the lock still held.  The gcc debug
// asyncio stress CI (X23_RCVBUF/SNDBUF=1) caught this in rpcthroughput as
//
//   PANIC: x23assert failure: old == kLocked
//     function void x23::TestLock::unlock()
//     x23::detail::WriteBuffer::Service::~Service()
//
// once glibc had scribbled its tcache pointers over the freed
// WriteBuffer.  Depending on the allocator for the signal makes for a
// flaky test, and x23assert is compiled out of release builds, so this
// test watches the memory itself: freed WriteBuffer-sized blocks are
// poisoned and quarantined rather than recycled, and any byte that
// changes afterwards was written after the free.

namespace {

constexpr std::size_t kWriteBufferSize = sizeof(x23::detail::WriteBuffer);
constexpr unsigned char kPoison = 0xa5;

// Freed blocks the size of a WriteBuffer, poisoned and deliberately not
// returned to the allocator so that nothing else can reuse them (and
// legitimately write to them) before we check.
struct Quarantine {
  static constexpr std::size_t kMax = 1024;

  std::atomic<std::size_t> claimed{0};
  void *blocks[kMax]{};

  // Called from ThreadPool workers as well as from main.
  void add(void *p) noexcept
  {
    std::size_t i = claimed.fetch_add(1, std::memory_order_relaxed);
    if (i >= kMax) {
      // Out of slots: hand the block back rather than leak it silently.
      // check() reports the overflow, since an unwatched free could be
      // the very one we care about.
      free(p);
      return;
    }
    std::memset(p, kPoison, kWriteBufferSize);
    blocks[i] = p;
  }

  // Returns true if every quarantined block is still untouched.  Call
  // once the CoroPoll and its ThreadPool are gone, so that nothing can be
  // adding blocks or touching a WriteBuffer concurrently.
  bool check() const
  {
    std::size_t n = claimed.load(std::memory_order_relaxed);
    if (n > kMax) {
      std::println(stderr, "INCONCLUSIVE: quarantine overflowed ({} blocks)",
                   n);
      return false;
    }
    if (!n) {
      std::println(stderr,
                   "INCONCLUSIVE: no WriteBuffer-sized block ({} bytes) was "
                   "ever freed, so nothing was watched",
                   kWriteBufferSize);
      return false;
    }
    bool ok = true;
    for (std::size_t i = 0; i < n; ++i) {
      auto *p = static_cast<unsigned char *>(blocks[i]);
      for (std::size_t j = 0; j < kWriteBufferSize; ++j)
        if (p[j] != kPoison) {
          std::println(stderr,
                       "BUG CONFIRMED: freed WriteBuffer-sized block {} was "
                       "written after free: byte {} is {:#04x}, not {:#04x}",
                       i, j, p[j], kPoison);
          ok = false;
          break;
        }
    }
    std::println(stderr, "checked {} quarantined block(s)", n);
    return ok;
  }
};

Quarantine quarantine;
std::atomic<bool> watching{false};

} // namespace

// Only the sized form quarantines: that is what `delete &wb` on a
// complete type compiles to, and it tells us the size without having to
// ask the allocator.  The unsized form is replaced alongside it (the two
// have to be replaced as a pair) but just frees, and check() reports it
// if no WriteBuffer ever reaches the quarantine.  The array and aligned
// forms keep the default deallocation functions, which pair with the
// default (malloc-based) allocation functions we are not replacing.
void
operator delete(void *p) noexcept
{
  free(p);
}

void
operator delete(void *p, std::size_t sz) noexcept
{
  if (p && sz == kWriteBufferSize && watching.load(std::memory_order_relaxed))
    quarantine.add(p);
  else
    free(p);
}

int
main() // NOLINT(bugprone-exception-escape)
{
  signal(SIGALRM, [](int) {
    const char msg[] = "TIMEOUT: shutdown did not finish\n";
    [[maybe_unused]] ssize_t n = write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(1);
  });
  alarm(10);

  // socketpair with small buffers: the write below cannot complete, and
  // nobody ever drains the peer, so WriteBuffer::flush() leaves work
  // behind and spawns the detached worker(), which then suspends in
  // write_wait() holding wb->service_lock_.
  int fds[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds))
    x23::Errno{}.raise("socketpair");
  x23::set_nonblock(fds[0]);
  int bufsz = 4096;
  setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));
  setsockopt(fds[1], SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));

  x23::ThreadPool pool(2);
  {
    x23::CoroPoll cp(pool);

    watching = true;
    x23::Buf data(4 << 20, uint8_t{'x'});
    if (x23::Errno err = cp.awrite(fds[0], std::move(data)))
      std::println(stderr, "awrite: {}", err);

    std::println(stderr, "destroying CoroPoll with a write still pending...");
    // ~CoroPoll runs here:
    //   reset(fds[0]) drops the BufHolder's WriteBuffer reference
    //   drain() resumes worker() with EBADF -> co_return
    //   the worker's reference was the last one, so the WriteBuffer is
    //   freed -- and must not be unlocked afterwards
  }
  // The pool has been drained by ~CoroPoll, so no worker can still be
  // touching a WriteBuffer by the time we check.
  watching = false;

  bool ok = quarantine.check();
  close(fds[0]);
  close(fds[1]);
  if (!ok)
    return 1;
  std::println(stderr, "OK -- no write to a freed WriteBuffer");
  return 0;
}
