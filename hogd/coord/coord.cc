#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <print>
#include <string>
#include <utility>

#include <x23/event.h>
#include <x23/fileio.h>
#include <x23/rpc.h>
#include <x23/threadpool.h>

#include "coord.hh"

using x23::Async;

namespace hogd {

namespace {

std::string progname = "coord";

x23::ThreadPool pool;
x23::CoroPoll cp(pool);

struct Coord {
  using rpc_service = CoordV1;
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  Async<void> null() { co_return; }
};
static_assert(is_CoordV1<Coord *, Async>);

[[noreturn]] void
usage(int err = STDERR_FILENO)
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
  // NOLINTNEXTLINE(misc-include-cleaner)
  while ((opt = getopt(argc, argv, "hs:p:")) != -1) {
    switch (opt) {
    case 'h':
      usage(0);
      break;
    case 'p':
      // NOLINTNEXTLINE(misc-include-cleaner)
      port = optarg;
      break;
    default:
      usage();
      break;
    }
  }

  // NOLINTNEXTLINE(misc-include-cleaner)
  if (optind != argc)
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

} // namespace

} // namespace hogd

int
main(int argc, char **argv) // NOLINT
{
  return hogd::main(argc, argv);
}
