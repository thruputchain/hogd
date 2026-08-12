
#include <cerrno>
#include <cstdint>
#include <print>
#include <semaphore>
#include <stdexcept>
#include <thread>

#include <sys/socket.h>
#include <unistd.h>

#include "x23/event.h"
#include "x23/sockio.h"

using namespace x23;
using std::string_literals::operator""s;

namespace {

class ManualCoroPoll : public CoroPoll {
public:
  explicit ManualCoroPoll(ThreadPool &pool)
    : CoroPoll(pool, no_loop_thread{})
  {}
};

void
test_poisoned_connect_propagates_error()
{
  ThreadPool pool(2);
  ManualCoroPoll cp(pool);

  auto bind_addr = HostPort{"127.0.0.1", uint16_t{0}}.to_addr(false);
  if (!bind_addr)
    throw std::runtime_error("could not construct loopback address");
  UniqueFd listenfd = stcplisten(*bind_addr);
  sockaddr_storage server_addr = local_addr(listenfd);

  UniqueFd fd(socket(server_addr.ss_family, SOCK_STREAM, 0));
  if (!fd)
    Errno{}.raise("socket");

  // ManualCoroPoll has no polling thread.  connect() therefore starts the
  // nonblocking TCP handshake and parks in write_wait(), but cannot observe the
  // writable edge on its own.
  auto pending = cp.connect(fd, &server_addr);

  // A successful accept proves that the TCP handshake itself has completed.
  // The client coroutine is nevertheless still parked because no CoroPoll loop
  // has run to deliver EPOLLOUT/POLLOUT.
  UniqueFd accepted(::accept(listenfd.get(), nullptr, nullptr));
  if (!accepted)
    Errno{}.raise("accept");

  // Simulate teardown aborting the pending network wait.  poison() completes
  // the registered write_wait() with EBADF.  connect() must propagate that
  // error rather than continuing and treating the already-established socket
  // as a successful connect.
  cp.poison(fd);
  auto result = pending();
  cp.reset(fd);

  if (result)
    throw std::runtime_error(
        "CoroPoll::connect returned success after its wait was poisoned");
  if (result.error().raw_code() != EBADF)
    throw std::runtime_error(
        "CoroPoll::connect did not propagate EBADF from poisoned wait");
}

} // namespace

void
test_matching_poison_generation(CoroPoll &cp)
{
  int fds[2];
  if (pipe(fds) == -1)
    Errno{}.raise("pipe");
  UniqueFd readfd(fds[0]);
  UniqueFd writefd(fds[1]);
  set_nonblock(readfd);

  uint64_t generation = cp.generation(readfd);
  auto token = cp.read_clear(readfd);
  std::binary_semaphore done(0);
  int result = -1;
  cp.read_wait(token)([&](Errno err) {
    result = err.raw_code();
    done.release();
  });

  cp.poison(readfd, generation);
  done.acquire();
  if (result != EBADF)
    throw std::runtime_error("matching generation did not poison fd");
  cp.reset(readfd);
}

void
test_stale_poison_generation(CoroPoll &cp)
{
  int fds[2];
  if (pipe(fds) == -1)
    Errno{}.raise("pipe");
  UniqueFd readfd(fds[0]);
  UniqueFd writefd(fds[1]);
  set_nonblock(readfd);

  uint64_t stale_generation = cp.generation(readfd);
  cp.reset(readfd);
  uint64_t generation = cp.generation(readfd);
  if (generation == stale_generation)
    throw std::runtime_error("fd generation did not change after reset");

  auto token = cp.read_clear(readfd);
  std::binary_semaphore done(0);
  int result = -1;
  cp.read_wait(token)([&](Errno err) {
    result = err.raw_code();
    done.release();
  });

  cp.poison(readfd, stale_generation);
  char c = 'x';
  if (write(writefd.get(), &c, sizeof(c)) != sizeof(c))
    Errno{}.raise("write");
  done.acquire();
  if (result != 0)
    throw std::runtime_error("stale generation poisoned recycled fd");
  if (cp.generation(readfd, false) != generation)
    throw std::runtime_error("stale poison changed fd generation");
  cp.reset(readfd);
}

int
main()
{
  setlinebuf(stdout);
  Buf b;

  test_poisoned_connect_propagates_error();

  ThreadPool pool(2);
  CoroPoll cp(pool);

  int fds[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == -1)
    Errno{}.raise("socketpair");

  std::binary_semaphore writeok(0);
  std::binary_semaphore exitok(0);

  std::jthread j([fd = UniqueFd(fds[0]), &writeok] {
    writeok.acquire();
    for (char c : "hello"s) {
      usleep(10000);
      write(fd.get(), &c, 1);
    }
  });

  set_nonblock(fds[1]);
  cp.read(fds[1], &b, 6)([&b, &exitok](auto res) {
    if (res)
      std::println("Got {} bytes: \"{}\"", *res,
                   std::move(b).reinterpret<Str>());
    else
      std::println("Got error: {}", res.error());
    exitok.release();
  });
  writeok.release();

  exitok.acquire();

  test_matching_poison_generation(cp);
  test_stale_poison_generation(cp);
}
