// hogdb.cc - handles initialization for hogdb

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <utility>

// XDR headers
#include "rpc_types.hh"
#include "sched_db.hh"

#include "hogd/base/cli.h"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/hogdb/hog_age_reconciliation.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/rpc_context.h"
#include "hogd/hogdb/rpc_from_auth.h"
#include "hogd/hogdb/rpc_from_exec.h"
#include "hogd/hogdb/rpc_from_gateway.h"
#include "hogd/hogdb/rpc_from_sched.h"
#include "hogd/hogdb/status.h"
#include "hogd/http/http.h"
#include "hogd/init.h"
#include "hogd/loadgen/loadgen_server.h"
#include "hogd/metrics/shared.h"
#include "hogd/protocol/block_compat.h"
#include "hogd/ramdb/db.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/version.h"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/sockio.h"

namespace hogd {

namespace {

auto
create_sched_db_client(x23::CoroPoll &cp, const std::string &coordinator_addr)
{
  x23::HostPort host_port;
  if (!host_port.parse(coordinator_addr)) {
    panic("Invalid coordinator address: {}", coordinator_addr);
  }

  return std::make_unique<
      SchedDb1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>>(
      make_rpc_client<SchedDb1>(host_port, cp, true)());
}

int
hogdb_init(std::string_view path, size_t ram_bytes,
           const std::string *coordinator_addr, std::string_view bind_ip,
           uint16_t exec_port, uint16_t mempool_port, uint16_t auth_port,
           uint16_t gateway_port, uint16_t metrics_port)
{
  HttpServer metrics_server;
  add_health_metrics_routes(metrics_server);
  const uint16_t actual_metrics_port =
      metrics_server.start_server(make_bind_host_port(bind_ip, metrics_port));
  info("PORT hogdb.metrics={}", actual_metrics_port);

  // check the database's integrity and find the highest valid LSN.
  DBStatus<uint64_t> lsn = Database::check(path, ram_bytes);
  if (!lsn) {
    critical("Database check failed: path '{}', {}", path, lsn.error());
    return EXIT_FAILURE;
  }
  info("database '{}' passed integrity check; current LSN is {}", path, *lsn);

  // create the database, loading up to the highest valid LSN.
  // TODO(abelay): When sharding is supported, this will become a separate step.
  DBStatus<std::unique_ptr<Database>> ramdb =
      Database::open_lsn(path, *lsn, ram_bytes);
  if (!ramdb) {
    critical("Could not create database: path '{}', {}", path, ramdb.error());
    return EXIT_FAILURE;
  }

  // create the RPC context.
  HogDBDAO db_tmp(RAMDB(std::move(*ramdb)));
  DbHash db_hash = db_tmp.compute_full_db_hash();

  std::unique_ptr<SchedDb1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>>
      sched_db_client;

  if (coordinator_addr) {
    sched_db_client =
        create_sched_db_client(default_coropoll(), *coordinator_addr);
  }
  else {
    warn("no coordinator address provided, skipping coordinator connection");
  }

  RPCContext ctx(std::move(db_tmp), std::move(sched_db_client));

  // load the tip metadata from the database (won't exist in a new database).
  HogDBStatus<DbTip> tip = ctx.get_db().read_dbtip();
  if (tip) {
    if (db_hash != tip->storage_long_hash) {
      critical("Root hash doesn't match DbTip");
      return EXIT_FAILURE;
    }
    ctx.set_tip(*tip);
    Health::instance().set_current_block_height(
        tip->value.proposal_header.block_height)();
    if (auto cost_model = ctx.get_db().read_cost_model(); cost_model) {
      ctx.initialize_cost_model(*cost_model);
    }
    else {
      critical("Failed to read cost model: {}", cost_model.error());
      return EXIT_FAILURE;
    }
  }
  else {
    CHECK_EQ(db_hash, DbHash{});
    ConsensusValue value{};
    value.proposal_header.block_height = -1;
    ctx.set_tip(DbTip{.id = std::numeric_limits<BlockTXID>::max(),
                      .storage_long_hash = {},
                      .value = std::move(value)});
    Health::instance().set_current_block_height(-1)();
  }

  HogDBStatus<void> reconcile_status = reconcile_startup_hog_ages(ctx.get_db());
  if (!reconcile_status.has_value()) {
    critical("Failed to reconcile startup hog ages: {}",
             reconcile_status.error());
    return EXIT_FAILURE;
  }

  // start the executor server.
  info("starting executor server.");
  HogDBExecutorServer exec_server(ctx);
  const uint16_t actual_exec_port =
      exec_server.start_server(make_bind_host_port(bind_ip, exec_port));
  CHECK(actual_exec_port == exec_port || exec_port == 0);
  info("PORT hogdb.exec={}", actual_exec_port);

  // start the mempool server.
  info("starting mempool server.");
  HogDBSchedServer mempool_server(ctx);
  mempool_server.max_message_size_ = LoadgenRpc::kRpcMaxMessageSize;
  const uint16_t actual_mempool_port =
      mempool_server.start_server(make_bind_host_port(bind_ip, mempool_port));
  CHECK(actual_mempool_port == mempool_port || mempool_port == 0);
  info("PORT hogdb.mempool={}", actual_mempool_port);

  // start the auth server.
  info("starting auth server.");
  HogDBAuthServer auth_server(ctx);
  const uint16_t actual_auth_port =
      auth_server.start_server(make_bind_host_port(bind_ip, auth_port));
  CHECK(actual_auth_port == auth_port || auth_port == 0);
  info("PORT hogdb.auth={}", actual_auth_port);

  info("starting gateway server.");
  HogDBGatewayServer gateway_server(ctx);
  const uint16_t actual_gateway_port =
      gateway_server.start_server(make_bind_host_port(bind_ip, gateway_port));
  CHECK(actual_gateway_port == gateway_port || gateway_port == 0);
  info("PORT hogdb.gateway={}", actual_gateway_port);

  if (coordinator_addr) {
    auto const &client = ctx.get_sched_db_client();
    assert(client);

    const std::string exec_address =
        format_advertised_address(bind_ip, actual_exec_port);
    const std::string sched_address =
        format_advertised_address(bind_ip, actual_mempool_port);
    const std::string auth_address =
        format_advertised_address(bind_ip, actual_auth_port);
    ShardCard card{.protocol = version::kHogdProtocol(),
                   .upid = {},
                   .block_height = 0,
                   .address_for_exec = exec_address.c_str(),
                   .address_for_sched = sched_address.c_str(),
                   .address_for_auth = auth_address.c_str()};

    client->scheddb_intro_shard(card)();
  }

  // block forever
  Health::instance().set_status(Health::Status::kOk)();
  info("hogdb initialization complete.");
  std::promise<void>().get_future().wait();

  return EXIT_SUCCESS;
}

} // namespace

} // namespace hogd

namespace {

[[noreturn]] void
usage(char *argv[])
{
  std::println("usage: {} --db-path <path> --ram-bytes <bytes> "
               "--bind-ip <ip> [--coordinator-addr <host:port>] "
               "[--executor-port <port>] [--mempool-port <port>] "
               "[--auth-port <port>] [--gateway-port <port>] "
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
  if (ret)
    return ret;
  hogd::info("starting hogdb!");

  std::string path;
  std::string bind_ip;
  std::string coordinator_addr_raw;
  std::optional<size_t> ram_bytes;
  uint16_t exec_port = 0;
  uint16_t mempool_port = 0;
  uint16_t auth_port = 0;
  uint16_t gateway_port = 0;
  uint16_t metrics_port = 0;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--metrics-port" && i + 1 < argc) {
      metrics_port =
          hogd::parse_port_arg("--metrics-port", argv[++i], argv, usage);
      continue;
    }
    if (arg == "--db-path" && i + 1 < argc) {
      path = argv[++i];
      continue;
    }
    if (arg == "--bind-ip" && i + 1 < argc) {
      bind_ip = argv[++i];
      continue;
    }
    if (arg == "--ram-bytes" && i + 1 < argc) {
      ram_bytes = hogd::parse_integer_arg<size_t>("--ram-bytes", argv[++i],
                                                  argv, usage, /*base=*/0);
      continue;
    }
    if (arg == "--coordinator-addr" && i + 1 < argc) {
      coordinator_addr_raw = argv[++i];
      continue;
    }
    if (arg == "--executor-port" && i + 1 < argc) {
      exec_port =
          hogd::parse_port_arg("--executor-port", argv[++i], argv, usage);
      continue;
    }
    if (arg == "--mempool-port" && i + 1 < argc) {
      mempool_port =
          hogd::parse_port_arg("--mempool-port", argv[++i], argv, usage);
      continue;
    }
    if (arg == "--auth-port" && i + 1 < argc) {
      auth_port = hogd::parse_port_arg("--auth-port", argv[++i], argv, usage);
      continue;
    }
    if (arg == "--gateway-port" && i + 1 < argc) {
      gateway_port =
          hogd::parse_port_arg("--gateway-port", argv[++i], argv, usage);
      continue;
    }
    usage(argv);
  }

  if (path.empty() || bind_ip.empty() || !ram_bytes.has_value()) {
    usage(argv);
  }

  const std::string *coordinator_addr =
      coordinator_addr_raw.empty() ? nullptr : &coordinator_addr_raw;

  return hogd::hogdb_init(path, *ram_bytes, coordinator_addr, bind_ip,
                          exec_port, mempool_port, auth_port, gateway_port,
                          metrics_port);
};
