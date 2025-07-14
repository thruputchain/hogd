// rpc_bench.cc - benchmarks for RPC framework

#include <cstddef>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/base/span.h"
#include "hogd/base/status.h"
#include "hogd/base/time.h"
#include "hogd/base/types.h"
#include "hogd/init.h"
#include "hogd/rpc/rpc.h"

namespace hogd {

namespace {

constexpr size_t kArgSize = 256;
constexpr size_t kRetSize = 8;

inline void
fail(Errno err)
{
  panic("Unexpected error: {}", err);
}

void
run_server(uint16_t port)
{
  auto handler =
      [](std::span<const std::byte> /*args*/) -> OwningSpan<const std::byte> {
    return string_as_bytes(std::string(kRetSize, '0'));
  };

  ErrnoStatus<std::shared_ptr<RPCServer>> server =
      RPCServer::create(port, handler);
  if (!server)
    fail(server.error());
  ErrnoStatus<void> ret = (*server)->listen();
  if (!ret)
    fail(ret.error());
}

void
run_client(std::string_view addr, int conns, int threads, int n)
{
  ErrnoStatus<RPCClient> client = RPCClient::dial(addr, conns);
  if (!client)
    fail(client.error());

  std::vector<std::jthread> workers;
  workers.reserve(threads);
  ElapsedTimer timer;
  for (int i = 0; i < threads; ++i) {
    workers.emplace_back([&c = *client, n] {
      std::vector<std::byte> args(kArgSize);
      for (int i = 0; i < n; ++i)
        c.call(std::span(args));
    });
  }
  for (std::jthread &th : workers)
    th.join();

  double tps = static_cast<double>(threads * n) / timer.elapsed();
  std::println("{}", tps);
}

} // namespace

} // namespace hogd

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  int ret = hogd::hogd_init(argc, argv);
  if (ret)
    return ret;
  hogd::info("starting rpc_bench!");

  auto print_usage = [] {
    std::println("usage:\tserver [port]");
    std::println("\tclient [ipaddr:port] [conns] [threads] [n]");
  };

  if (argc < 2) {
    print_usage();
    return 1;
  }

  if (std::string(argv[1]) == "client") {
    if (argc < 6) {
      print_usage();
      return 1;
    }
    int conns = std::stoi(argv[3], nullptr, 0);
    int threads = std::stoi(argv[4], nullptr, 0);
    int n = std::stoi(argv[5], nullptr, 0);
    hogd::run_client(argv[2], conns, threads, n);
    return 0;
  }

  if (std::string(argv[1]) == "server") {
    if (argc < 3) {
      print_usage();
      return 1;
    }
    uint16_t port = std::stoi(argv[2], nullptr, 0); // NOLINT
    hogd::run_server(port);
    return 0;
  }

  print_usage();
  return 1;
}
