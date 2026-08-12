// Regression test for issue #3318: WriteBuffer::Service must not unlock
// wb.service_lock_ after the WriteBuffer that lock lives in has been freed.
//
// Service holds a std::unique_lock on WriteBuffer::service_lock_, a TestLock
// embedded in the WriteBuffer itself.  Service::worker() is a coroutine that
// keeps the buffer alive with a RefPtr *local*, but a coroutine's parameter
// copies -- the Service, and therefore the unique_lock -- are destroyed with
// the frame, which happens *after* the body's locals.  So at any co_return
// that still owns the lock, the local drops the last reference to the
// WriteBuffer first, and only then is the lock released, on freed memory.
//
// reset() makes that the ordinary teardown order: buf_clear() drops the fd
// table's writer reference before setmode() -> docbs() wakes the parked
// writer, so a woken worker routinely holds the last reference.
//
// Two co_returns in worker() can be reached with the lock held.  Each gets a
// part below; both are guarded by the same fix (hold the reference in a
// Service *member* declared ahead of lk_, so reverse-order member destruction
// releases the lock while the buffer is still alive).
//
//   Part 1 -- woken with EBADF, before the retry.  Fixed in #3234 by
//             unlocking explicitly on that path; kept here so a later
//             refactor cannot silently reintroduce it.
//   Part 2 -- woken writable, but the retried write() fails and poisons the
//             fd.  Still unfixed: that co_return leaves the lock held.
//
// Detection: on an unpatched tree the unlock lands in a block glibc has
// already recycled, so the exchange() returns neither kLocked nor
// kLockedWantNotify and x23assert(old == kLocked) aborts.  That assertion is
// compiled out under NDEBUG -- build without NDEBUG, or build with ASAN,
// which reports the same stack as a heap-use-after-free.
//
// Sibling of poll_writer_zombie.cc / writebuf_fd_poison.cc: a socketpair with
// a send buffer too small to drain, a parked writer, and a reset().  No TCP
// and no timing races -- the single pool thread is held busy so the worker
// resumes only where we say it does.

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <print>
#include <semaphore>

#include <sys/socket.h>
#include <unistd.h>

#include <x23/err.h>
#include <x23/event.h>
#include <x23/sockio.h>
#include <x23/threadpool.h>
#include <x23/vec.h>

using namespace x23;

namespace {

// Fill the socket send buffer so the next write is guaranteed to EAGAIN and
// park a write worker.
void
fill_send_buffer(BorrowedFd fd)
{
  std::array<std::uint8_t, 4096> junk{};
  for (;;) {
    ssize_t n = ::write(fd.get(), junk.data(), junk.size());
    if (n >= 0)
      continue;
    if (errno == EAGAIN)
      return;
    Errno{}.raise("prefill write");
  }
}

// A socketpair whose sending half cannot drain, plus a pool with its one
// thread parked, so nothing the CoroPoll schedules runs until we say so.
struct ParkedWriter {
  ThreadPool pool{1};
  std::binary_semaphore blocker_running{0};
  std::binary_semaphore release_pool{0};
  CoroPoll cp{pool};
  UniqueFd server;
  UniqueFd peer;

  ParkedWriter()
  {
    pool.spawn([this] {
      blocker_running.release();
      release_pool.acquire();
    });
    blocker_running.acquire();

    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == -1)
      Errno{}.raise("socketpair");
    server = UniqueFd{fds[0]};
    peer = UniqueFd{fds[1]};
    set_nonblock(server);
    set_nonblock(peer);

    int sndbuf = 4096;
    if (setsockopt(server.get(), SOL_SOCKET, SO_SNDBUF, &sndbuf,
                   sizeof(sndbuf)) == -1)
      Errno{}.raise("setsockopt(SO_SNDBUF)");
    fill_send_buffer(server);
  }

  // Enqueue a payload big enough to flush immediately rather than sit in the
  // delayed-write buffer.  The send buffer is full, so Service::write() gets
  // EAGAIN and spawns the detached worker, which parks in write_wait()
  // holding the lock on server's WriteBuffer::service_lock_.
  [[nodiscard]] bool park()
  {
    Buf payload(256 * 1024, std::uint8_t{0xc7});
    if (Errno err = cp.dwrite(server, std::move(payload))) {
      std::println(stderr, "dwrite returned unexpected error: {}", err);
      return false;
    }
    return true;
  }

  // Let the parked worker run, and wait for it to finish -- aborting on the
  // way out, if the bug is present.
  void run_worker()
  {
    release_pool.release();
    pool.drain();
  }
};

// Part 1: the worker is woken with EBADF and gives up without retrying.
bool
ebadf_wakeup()
{
  ParkedWriter w;
  if (!w.park()) {
    w.release_pool.release();
    return false;
  }

  // Ordinary teardown: buf_clear() drops the fd table's reference to the
  // WriteBuffer, then the mode change wakes the parked worker with EBADF.
  // The coroutine's own RefPtr is now the last reference.
  w.cp.reset(w.server);

  w.run_worker();
  std::println("OK (part 1: EBADF wakeup)");
  return true;
}

// Part 2: the worker is woken writable, retries, and the write fails.
bool
failed_write_after_wakeup()
{
  ParkedWriter w;
  if (!w.park()) {
    w.release_pool.release();
    return false;
  }

  // Wake the worker with success rather than EBADF.  abort() hands the
  // coroutine to the pool synchronously, and the pool is parked, so the
  // wakeup is already committed before anything below runs -- no race.
  w.cp.abort(w.server, true, 0);

  // The peer goes away, so the write the worker is about to retry fails
  // (EPIPE) and poisons the fd, which is the co_return that still holds the
  // lock.
  w.peer.reset();

  // Ordinary teardown again: drop the fd table's reference, leaving the
  // parked coroutine holding the last one.
  w.cp.reset(w.server);

  w.run_worker();
  std::println("OK (part 2: write failure after wakeup)");
  return true;
}

} // namespace

int
main(int argc, char **argv) // NOLINT(bugprone-exception-escape)
{
  setlinebuf(stdout);

  // The freed WriteBuffer has to be observably clobbered for the assertion in
  // TestLock::unlock() to fire every time.  MALLOC_PERTURB_ has to be set
  // before the first allocation, so re-exec ourselves once with it set rather
  // than relying on whatever glibc happens to leave behind in a free chunk.
  if (const char *p = getenv("MALLOC_PERTURB_"); !p || !strcmp(p, "0")) {
    setenv("MALLOC_PERTURB_", "165", 1);
    execv(argv[0], argv);
    Errno{}.raise("execv"); // only reached if execv failed
  }
  (void)argc;

  if (!ebadf_wakeup() || !failed_write_after_wakeup())
    return 2;

  std::println("OK -- write worker released service_lock_ before dropping "
               "the last WriteBuffer reference");
  return 0;
}
