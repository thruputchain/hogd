
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
