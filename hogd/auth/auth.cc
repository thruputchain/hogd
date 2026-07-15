#include <cstdint>
#include <cstdlib>
#include <format>
#include <future>
#include <print>
#include <string>
#include <utility>

#include "auth_card.hh"
#include "auth_rpc.hh"
#include "hogd/auth/auth_server.h"
#include "hogd/auth/dbauth_client.h"
#include "hogd/base/cli.h"
#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/http/http.h"
#include "hogd/init.h"
#include "hogd/loadgen/loadgen_server.h"
#include "hogd/metrics/shared.h"
#include "hogd/version.h"
#include "sched_db.hh"
#include "x23/sockio.h"

namespace hogd {

namespace {

int
auth_init(const std::string &mempool_address, const std::string &bind_ip,
          uint16_t db_port, uint16_t mempool_port, uint16_t metrics_port)
{
  HttpServer metrics_server;
  add_health_metrics_routes(metrics_server);
  const uint16_t actual_metrics_port =
      metrics_server.start_server(make_bind_host_port(bind_ip, metrics_port));
  info("PORT auth.metrics={}", actual_metrics_port);

  x23::HostPort host_port;
  if (!host_port.parse(mempool_address)) {
    panic("Invalid mempool address: {}", mempool_address);
  }

  auto mempool_client_res =
      make_rpc_client<MempoolAuth1>(host_port, /*retry=*/true)();
  if (!mempool_client_res)
    panic("auth connect to mempool failed: {}",
          mempool_client_res.error().error_code().message());
  sched_auth_client_t mempool_client = std::move(*mempool_client_res);

  RpcServer<AuthDBRpc> auth_db_rpc;
  auth_db_rpc->max_message_size_ = hogd::LoadgenRpc::kRpcMaxMessageSize;
  auto [actual_db_port, db_lfd] =
      auth_db_rpc.bind_server(make_bind_host_port(bind_ip, db_port));
  CHECK(actual_db_port == db_port || db_port == 0);
  info("PORT auth.db={}", actual_db_port);

  RpcServer<AuthMempoolRpc> auth_mempool_rpc;
  auth_mempool_rpc->max_message_size_ = hogd::LoadgenRpc::kRpcMaxMessageSize;
  auto [actual_mempool_port, mempool_lfd] =
      auth_mempool_rpc.bind_server(make_bind_host_port(bind_ip, mempool_port));
  CHECK(actual_mempool_port == mempool_port || mempool_port == 0);
  info("PORT auth.mempool={}", actual_mempool_port);

  const std::string auth_db_address =
      format_advertised_address(bind_ip, actual_db_port);
  const std::string auth_mempool_address =
      format_advertised_address(bind_ip, actual_mempool_port);

  AuthCard auth_card{.protocol = version::kHogdProtocol(),
                     .upid = {},
                     .address_for_db = auth_db_address.c_str(),
                     .address_for_mempool = auth_mempool_address.c_str()};
  IntroAuthResult intro = mempool_client.mempoolauth_intro_auth(auth_card)();
  if (intro.shards().empty()) {
    panic("No db shards registered with mempool");
  }
  ShardCard &shard = intro.shards()[0];
  const std::string address_for_auth = shard.address_for_auth.c_str();
  auto dbauth_res = make_dbauth_client(address_for_auth)();
  if (!dbauth_res)
    panic("auth connect to dbauth failed: {}",
          dbauth_res.error().error_code().message());
  dbauth_client_t dbauth_client = std::move(*dbauth_res);
  AuthServer auth_server(std::move(dbauth_client));

  auth_server.set_mempool_client(std::move(mempool_client));
  auth_db_rpc->impl()->set_auth_server(auth_server);
  auth_mempool_rpc->impl()->set_auth_server(auth_server);

  auth_db_rpc.start_accepting(std::move(db_lfd));
  auth_mempool_rpc.start_accepting(std::move(mempool_lfd));

  auth_server.subscribe_to_auth_data_changes(auth_card)();

  Health::instance().set_status(Health::Status::kOk)();
  info("auth server initialization complete.");
  std::promise<void>().get_future().wait();
  return EXIT_SUCCESS;
}

} // namespace

} // namespace hogd

namespace {

[[noreturn]] void
usage(char *argv[])
{
  std::println("usage: {} --mempool-addr <host:port> --bind-ip <ip> "
               "[--db-port <port>] [--mempool-port <port>] "
               "[--metrics-port <port>] [--version]",
               argv[0]);
  exit(EXIT_FAILURE);
}

} // namespace

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  if (hogd::print_version_and_exit_if_requested(argc, argv)) {
    return EXIT_SUCCESS;
  }
  int ret = hogd::hogd_init(argc, argv);
  if (ret) {
    return ret;
  }
  hogd::info("starting auth server!");

  std::string mempool_address;
  std::string bind_ip;
  uint16_t db_port = 0;
  uint16_t mempool_port = 0;
  uint16_t metrics_port = 0;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--metrics-port" && i + 1 < argc) {
      metrics_port =
          hogd::parse_port_arg("--metrics-port", argv[++i], argv, usage);
      continue;
    }
    if (arg == "--mempool-addr" && i + 1 < argc) {
      mempool_address = argv[++i];
      continue;
    }
    if (arg == "--bind-ip" && i + 1 < argc) {
      bind_ip = argv[++i];
      continue;
    }
    if (arg == "--db-port" && i + 1 < argc) {
      db_port = hogd::parse_port_arg("--db-port", argv[++i], argv, usage);
      continue;
    }
    if (arg == "--mempool-port" && i + 1 < argc) {
      mempool_port =
          hogd::parse_port_arg("--mempool-port", argv[++i], argv, usage);
      continue;
    }
    usage(argv);
  }

  if (mempool_address.empty() || bind_ip.empty()) {
    usage(argv);
  }

  return hogd::auth_init(mempool_address, bind_ip, db_port, mempool_port,
                         metrics_port);
}
