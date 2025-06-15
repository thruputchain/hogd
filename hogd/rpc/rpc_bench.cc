// rpc_bench.cc - benchmarks for RPC framework

#include <cstddef>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
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

  RPCServer server(port, handler);
  ErrnoStatus<void> ret = server.listen();
  if (!ret)
    fail(ret.error());
}

void
run_client(std::string_view addr, int conns, int threads_per_conn, int n)
{
  std::vector<std::unique_ptr<RPCClient>> clients;
  clients.reserve(conns);

  for (int i = 0; i < conns; ++i) {
    ErrnoStatus<std::unique_ptr<RPCClient>> ret = RPCClient::dial(addr);
    if (!ret)
      fail(ret.error());
    clients.emplace_back(std::move(*ret));
  }

  std::vector<std::jthread> workers;
  workers.reserve(conns * threads_per_conn);
  ElapsedTimer timer;
  for (std::unique_ptr<RPCClient> &c : clients) {
    for (int i = 0; i < threads_per_conn; ++i) {
      workers.emplace_back([&c, n] {
        std::vector<std::byte> args(kArgSize);
        for (int i = 0; i < n; ++i)
          c->call(std::span(args)).wait();
      });
    }
  }
  for (std::jthread &th : workers)
    th.join();

  int total = conns * threads_per_conn * n;
  double tps = static_cast<double>(total) / timer.elapsed();
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
    std::println("\tclient [ipaddr:port] [conns] [threads_per_conn] [n]");
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
    int threads_per_conn = std::stoi(argv[4], nullptr, 0);
    int n = std::stoi(argv[5], nullptr, 0);
    hogd::run_client(argv[2], conns, threads_per_conn, n);
    return 0;
  }

  if (std::string(argv[1]) == "server") {
    if (argc < 3) {
      print_usage();
      return 1;
    }
    uint16_t port = std::stoi(argv[2], nullptr, 0);
    hogd::run_server(port);
    return 0;
  }

  print_usage();
  return 1;
}
