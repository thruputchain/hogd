// Regression test for issue #3233: a delayed-write (dwrite) worker that is
// suspended at write_wait() must NOT write to the fd after the fd has been
// poisoned/reset.  Otherwise the stale bytes can later land on a different
// connection that recycles the same integer fd (RPC response injection).
//
// poison() wakes the suspended worker with EBADF; with the fix the worker
// stops instead of retrying the write, so no stale bytes reach the peer.

#include <array>
#include <cerrno>
#include <cstdint>
#include <print>
#include <semaphore>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <x23/err.h>
#include <x23/event.h>
#include <x23/sockio.h>
#include <x23/threadpool.h>
#include <x23/vec.h>

using namespace x23;

static void
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

static void
drain(BorrowedFd fd)
{
  std::array<std::uint8_t, 4096> buf{};
  for (;;) {
    ssize_t n = ::read(fd.get(), buf.data(), buf.size());
    if (n > 0)
      continue;
    return; // EOF or EAGAIN: nothing more currently readable
  }
}

static bool
wait_readable(BorrowedFd fd, int timeout_ms)
{
  pollfd pfd{.fd = fd.get(), .events = POLLIN, .revents = 0};
  int rc = ::poll(&pfd, 1, timeout_ms);
  if (rc == -1)
    Errno{}.raise("poll");
  return rc > 0 && (pfd.revents & POLLIN);
}

int
main() // NOLINT(bugprone-exception-escape)
{
  setlinebuf(stdout);

  // Single pool thread that we keep blocked so we control exactly when the
  // detached write worker is allowed to resume.
  ThreadPool pool(1);
  std::binary_semaphore blocker_running(0);
  std::binary_semaphore release_pool(0);
  pool.spawn([&] {
    blocker_running.release();
    release_pool.acquire();
  });
  blocker_running.acquire();

  CoroPoll cp(pool);

  int fds[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == -1)
    Errno{}.raise("socketpair");
  UniqueFd server{fds[0]}; // the fd the RPC response would be written to
  UniqueFd peer{fds[1]};
  set_nonblock(server);
  set_nonblock(peer);

  int sndbuf = 4096;
  if (setsockopt(server.get(), SOL_SOCKET, SO_SNDBUF, &sndbuf,
                 sizeof(sndbuf)) == -1)
    Errno{}.raise("setsockopt(SO_SNDBUF)");

  fill_send_buffer(server);

  // Enqueue a delayed write large enough to flush immediately.  The send
  // buffer is full, so the write EAGAINs and a detached worker suspends at
  // write_wait().
  constexpr std::uint8_t kMarker = 0xc7;
  constexpr std::size_t kPayloadSize = 256 * 1024;
  Buf payload(kPayloadSize, kMarker);
  if (Errno err = cp.dwrite(server, std::move(payload))) {
    std::println(stderr, "dwrite returned unexpected error: {}", err);
    release_pool.release();
    return 2;
  }

  // Peer disconnects: the stream is poisoned and reset, as in RpcStreamImpl
  // teardown.  This wakes the suspended worker with EBADF.
  cp.poison(server);
  cp.reset(server);

  // Make the fd writable again so a buggy worker WOULD be able to push the
  // stale payload out to the peer.
  drain(peer);

  // Allow the worker to resume.
  release_pool.release();

  // With the fix the worker stops on EBADF and writes nothing.  Any marker
  // bytes arriving on the peer mean the stale write escaped.
  if (wait_readable(peer, 1000)) {
    std::array<std::uint8_t, 16> got{};
    ssize_t n = ::read(peer.get(), got.data(), got.size());
    std::println(stderr,
                 "BUG CONFIRMED: stale dwrite payload wrote {} byte(s) after "
                 "poison/reset (first byte {:02x})",
                 n, n > 0 ? unsigned(got[0]) : 0U);
    return 1;
  }

  std::println("OK -- no stale bytes written after poison/reset");
  cp.reset(server);
  return 0;
}
