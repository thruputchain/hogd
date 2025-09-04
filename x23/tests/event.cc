
#include <print>
#include <thread>

#include <sys/socket.h>
#include <unistd.h>

#include "x23/event.h"
#include "x23/sockio.h"

using namespace x23;
using std::string_literals::operator""s;

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
}
