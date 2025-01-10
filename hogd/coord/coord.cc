#include <print>
#include <string>
#include <utility>

#include <getopt.h> //NOLINT
#include <stdio.h>  //NOLINT
#include <stdlib.h> //NOLINT
#include <unistd.h> //NOLINT

#include <x23/event.h>
#include <x23/fileio.h>
#include <x23/rpc.h>
#include <x23/threadpool.h>

#include "coord.hh"

using x23::Async;

std::string progname = "coord";

x23::ThreadPool pool;
x23::CoroPoll cp(pool);

namespace hogd {

struct Coord {
  using rpc_service = CoordV1;
  static Async<void> null() { co_return; }
};
static_assert(is_CoordV1<Coord *, Async>);

[[noreturn]] void
usage(int err = 2)
{
  std::println(err == 0 ? stdout : stderr, "Usage: {} [-p <port>]", progname);
  exit(err);
}

int
main(int argc, char **argv)
{
  if (argc > 0)
    progname = std::filesystem::path(argv[0]).filename();
  std::string port = std::to_string(kCoordPort);

  int opt;
  while ((opt = getopt(argc, argv, "hs:p:")) != -1) { // NOLINT
    switch (opt) {
    case 'h':
      usage(0);
      break;
    case 'p':
      port = optarg; // NOLINT
      break;
    default:
      usage();
      break;
    }
  }

  if (optind != argc) // NOLINT
    usage();

  auto lfd = x23::tcplisten(atoi(port.c_str()));
  if (!lfd) {
    perror("listen");
    return EXIT_FAILURE;
  }
  std::println(stderr, "listening on port {}", port);

  Coord server;
  [&server, lfd{std::move(lfd)}]() -> Async<void> {
    while (auto c = co_await cp.accept(lfd))
      rpc_stream_logged(cp, *c).server(&server);
  }()();
  return EXIT_SUCCESS;
}

} // namespace hogd

int
main(int argc, char **argv) // NOLINT
{
  return hogd::main(argc, argv);
}
