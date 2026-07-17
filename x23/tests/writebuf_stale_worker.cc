// Regression test for the fd-recycle variant of issue #1334 (fixed by
// #3234): a WriteBuffer::Service::worker() parked in write_wait() must not
// write its pending bytes after the stream dies -- even when the fd
// *number* has been recycled by a new connection and is valid again by the
// time the worker resumes.
//
// The worker (event.cc) is a Detached coroutine that holds only a
// RefPtr<WriteBuffer> and the raw fd number -- no reference to the
// RpcStream that spawned the write, and no ownership of the fd.  Before
// the fix it discarded the Errno from write_wait, so the EBADF wakeup
// queued by the stream's teardown (cp.reset + close, as in ~RpcStreamImpl)
// was ignored: the worker resumed and did write_clear() + writev() on
// whatever connection had recycled the fd number, injecting stale bytes
// mid-stream and desyncing the victim's RPC record framing.  The victim's
// reader then waits forever for a record tail that never arrives -- the
// rpcstress_teardown CI hang signature.
//
// writebuf_fd_poison covers the poisoned-but-still-dead-fd case on the
// same socket.  This test pins the harder variant: the fd is genuinely
// closed and its number recycled (dup2) onto a different socket, so when
// the worker resumes the fd is *valid* -- it just belongs to somebody
// else.  A worker that re-checked fd validity instead of heeding the
// write_wait error would pass writebuf_fd_poison but fail here.
//
// Staging (deterministic):
//
//   1. Clog the single pool thread so queued callbacks cannot run.
//   2. Socket A: shrink the send buffer, fill it until EAGAIN, then
//      awrite() 32KB of 0xAA marker -> a Service worker parks in
//      write_wait(A) with the marker bytes pending.
//   3. Kill A exactly like ~RpcStreamImpl: cp.reset(A); close(A).  The
//      worker's EBADF wakeup is now queued behind the clogged pool thread.
//   4. dup2() a fresh socketpair B onto A's fd number (deterministic fd
//      recycling).
//   5. Unclog the pool; the worker's wakeup runs now.
//   6. Watch B's peer: any 0xAA bytes mean the stale worker wrote into a
//      recycled fd owned by another socket.
//
// Exits 1 (test failure) when the corruption reproduces, 0 when the worker
// correctly stops.  Verified on the poll backend; the worker logic in
// event.cc is shared with the epoll backend.

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <print>
#include <thread>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <x23/async.h>
#include <x23/err.h>
#include <x23/event.h>
#include <x23/sockio.h>
#include <x23/threadpool.h>
#include <x23/vec.h>

using namespace x23;

int
main() // NOLINT(bugprone-exception-escape)
{
  setlinebuf(stdout);

  ThreadPool pool(1);
  CoroPoll cp(pool);

  // 1. Clog the single pool thread so queued callbacks cannot run yet.
  std::atomic<bool> gate{false};
  std::atomic<bool> clogged{false};
  [](ThreadPool &p, std::atomic<bool> &gate,
     std::atomic<bool> &clogged) -> Detached {
    co_await p.bg();
    clogged = true;
    while (!gate)
      std::this_thread::yield();
  }(pool, gate, clogged);
  while (!clogged)
    std::this_thread::yield();

  // 2. Socket A with a full send buffer, so awrite parks a worker.
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) {
    perror("socketpair");
    return 2;
  }
  int a_fd = sv[0];
  int a_peer = sv[1];
  set_nonblock(a_fd);
  int tiny = 1;
  setsockopt(a_fd, SOL_SOCKET, SO_SNDBUF, &tiny, sizeof(tiny));
  char junk[1024];
  memset(junk, 'J', sizeof junk);
  while (write(a_fd, junk, sizeof junk) > 0)
    ;
  std::println("A (fd {}): send buffer full (errno {})", a_fd, errno);

  Buf marker(32 * 1024, uint8_t{0xAA});
  Errno err = cp.awrite(a_fd, std::move(marker));
  std::println("awrite(A) -> {} (worker now parked in write_wait)",
               err ? "error" : "0");

  // 3. Stream death, exactly like ~RpcStreamImpl: reset then close.  The
  // worker's EBADF wakeup is queued behind the clogged pool thread.
  cp.reset(a_fd);
  close(a_fd);

  // 4. Deterministically recycle A's fd number onto a fresh socket B.
  int sv2[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv2)) {
    perror("socketpair");
    return 2;
  }
  int victim = sv2[0];
  int victim_peer = sv2[1];
  if (victim != a_fd) {
    if (dup2(victim, a_fd) < 0) {
      perror("dup2");
      return 2;
    }
    close(victim);
    victim = a_fd;
  }
  set_nonblock(victim);
  std::println("B: fresh socket now owns fd {}", victim);

  // 5. Unclog the pool; the worker's EBADF wakeup runs now.
  gate = true;

  // 6. A correct worker heeds the write_wait error and writes nothing;
  // give a buggy one up to a second to inject A's marker bytes into B.
  uint8_t rbuf[64 * 1024];
  ssize_t n = 0;
  size_t naa = 0;
  pollfd pfd{.fd = victim_peer, .events = POLLIN, .revents = 0};
  if (poll(&pfd, 1, 1000) > 0 && (pfd.revents & POLLIN) != 0) {
    n = read(victim_peer, rbuf, sizeof rbuf);
    for (ssize_t i = 0; i < n; i++)
      if (rbuf[i] == 0xAA)
        ++naa;
  }
  std::println("read from B's peer: {} bytes, {} of them stale 0xAA marker", n,
               naa);

  ssize_t apn;
  size_t atot = 0;
  while ((apn = read(a_peer, rbuf, sizeof rbuf)) > 0)
    atot += apn;
  std::println("(A's peer had {} bytes buffered)", atot);

  bool corrupted = naa > 0;
  if (corrupted)
    std::println("*** CORRUPTION: stale WriteBuffer worker wrote {} bytes "
                 "into a recycled fd owned by another socket ***",
                 naa);
  else
    std::println("OK -- worker wrote nothing into the recycled fd");

  // _exit: tearing down the CoroPoll after deliberately recycling an fd out
  // from under it is not the point of this test and can itself wedge.
  fflush(stdout);
  _exit(corrupted ? 1 : 0);
}
