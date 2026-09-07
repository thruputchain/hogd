#include <cerrno>
#include <format>
#include <semaphore>
#include <stdexcept>
#include <string>

#include <sys/socket.h>
#include <unistd.h>

#include <x23/rpc.h>

using namespace x23;

namespace {
struct TestStream : detail::RpcStreamImpl {
  TestStream(CoroPoll &cp, UniqueFd fd) : RpcStreamImpl(cp, std::move(fd)) {}
  using RpcStreamImpl::send_request;
  void log_message(std::string) override {}
};

void
check_flush(int cancel)
{
  ThreadPool pool(1);
  CoroPoll cp(pool);
  std::binary_semaphore running(0), release(0);
  pool.spawn([&] {
    running.release();
    release.acquire();
  });
  running.acquire();

  int fds[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == -1)
    Errno{}.raise("socketpair");
  UniqueFd peer{fds[1]};
  set_nonblock(peer);
  RefPtr<TestStream> stream{*new TestStream(cp, UniqueFd{fds[0]})};
  Buf record;
  record.push_back(0xc7);
  stream->send_request(std::move(record));

  // No reader is running: the queued flush must itself observe invalidation,
  // rather than relying on a reader callback eventually calling fail().
  if (cancel == 1)
    cp.poison(stream->get_fd());
  else if (cancel == 2)
    cp.reset(stream->get_fd());
  release.release();
  pool.drain();

  unsigned char byte = 0;
  auto n = ::read(peer.get(), &byte, 1);
  if (cancel ? (n != -1 || errno != EAGAIN) : (n != 1 || byte != 0xc7))
    throw std::runtime_error(std::format(
        "queued RPC flush failed: cancel={}, read={}, errno={}", cancel, n,
        errno));
}
} // namespace

int
main()
{
  check_flush(0); // A lone request needs no later request to trigger a flush.
  check_flush(1);
  check_flush(2);
}
