#include <unistd.h>

#include <cstdlib>
#include <expected> // NOLINT
#include <print>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <x23/event.h>
#include <x23/rpc.h>
#include <x23/threadpool.h>

#include "coord.hh"

using x23::Async;

namespace hogd {

namespace {

std::string progname = "coord_cli";

x23::ThreadPool pool;
x23::CoroPoll cp(pool);

[[noreturn]] void
usage(int err = STDERR_FILENO)
{
  std::println(err == 0 ? stdout : stderr,
               "Usage: {} [-s <server>] [-p port] <method> [<arg> ...]",
               progname);
  exit(err);
}

// TODO(amb): this was already fixed in clang's main branch
// NOLINTNEXTLINE(misc-include-cleaner)
Async<std::expected<x23::RpcClient<CoordV1, x23::RpcLogger>, std::system_error>>
make_client(std::string host, std::string port)
{
  auto fd = co_await co_await cp.tcpconnect(std::move(host), std::move(port));
  co_return rpc_stream_logged(cp, fd).client(CoordV1{});
}

Async<int>
go(std::string host, std::string port, std::vector<const char *> args)
{
  using namespace std::string_literals;

  auto c = co_await make_client(std::move(host), std::move(port));
  if (!c) {
    std::println("{}", c.error().what());
    co_return EXIT_FAILURE;
  }

  std::string cmd = args[0];
  if (cmd == "null") {
    co_await c->null();
  }
  else {
    std::println(stderr, "unknown command {}", cmd);
    co_return 2;
  }
  co_return EXIT_SUCCESS;
}

int
main(int argc, char **argv)
{
  if (argc > 0)
    progname = std::filesystem::path(argv[0]).filename();

  std::string port = std::to_string(kCoordPort);
  std::string host = "localhost";

  int opt;
  // NOLINTNEXTLINE(misc-include-cleaner)
  while ((opt = getopt(argc, argv, "hs:p:")) != -1) {
    switch (opt) {
    case 'h':
      usage(0);
      break;
    case 's':
      host = optarg; // NOLINT(misc-include-cleaner)
      break;
    case 'p':
      port = optarg; // NOLINT(misc-include-cleaner)
      break;
    default:
      usage();
      break;
    }
  }

  if (optind >= argc) // NOLINT(misc-include-cleaner)
    usage();

  x23::RpcLogger::default_logfd = STDOUT_FILENO;
  return go(std::move(host), std::move(port),
            std::vector<const char *>(argv + optind, argv + argc))();
}

} // namespace

} // namespace hogd

int
main(int argc, char **argv) // NOLINT
{
  return hogd::main(argc, argv);
}
