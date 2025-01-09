#include "coord.hh"
#include <x23/fileio.h>
#include <x23/rpc.h>

using namespace hogd;
using x23::Async;

std::string progname = "coord_cli";

x23::ThreadPool pool;
x23::CoroPoll cp(pool);

[[noreturn]] void
usage(int err = 2)
{
  std::println(err == 0 ? stdout : stderr,
               "Usage: {} [-s <server>] [-p port] <method> [<arg> ...]",
               progname);
  exit(err);
}

Async<std::expected<x23::RpcClient<CoordV1, x23::RpcLogger>, std::system_error>>
make_client(std::string host, std::string port)
{
  auto fd = co_await co_await cp.tcpconnect(host, port);
  co_return rpc_stream_logged(cp, fd).client(CoordV1{});
}

Async<int>
go(std::string host, std::string port, std::vector<const char *> args)
{
  using namespace std::string_literals;

  auto c = co_await make_client(host, port);
  if (!c) {
    std::println("{}", c.error().what());
    co_return 1;
  }

  std::string cmd = args[0];
  if (cmd == "null") {
    co_await c->null();
  }
  else {
    std::println(stderr, "unknown command {}", cmd);
    co_return 2;
  }
  co_return 0;
}

int
main(int argc, char **argv)
{
  if (argc > 0)
    progname = std::filesystem::path(argv[0]).filename();

  std::string port = std::to_string(kCoordPort);
  std::string host = "localhost";

  int opt;
  while ((opt = getopt(argc, argv, "hs:p:")) != -1) {
    switch (opt) {
    case 'h':
      usage(0);
      break;
    case 's':
      host = optarg;
      break;
    case 'p':
      port = optarg;
      break;
    default:
      usage();
      break;
    }
  }

  if (optind >= argc)
    usage();

  x23::RpcLogger::default_logfd = 1;
  return go(host, port,
            std::vector<const char *>(argv + optind, argv + argc))();
}
