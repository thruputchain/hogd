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
#include "types.hh"

#include "hogd/base/cli.h"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/rpc_message_size.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"             // IWYU pragma: keep
#include "hogd/db_shard_routing/sharding.h" // kShardFunctionId
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
#include "hogd/metrics/shared.h"
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

  auto client = make_rpc_client<SchedDb1>(host_port, cp, true)();
  if (!client)
    panic("hogdb connect to coordinator failed: {}",
          client.error().error_code().message());
  return std::make_unique<
      SchedDb1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>>(
      std::move(*client));
}

int
hogdb_init(std::string_view path, size_t ram_bytes,
           const std::string *coordinator_addr, std::string_view bind_ip,
           uint16_t exec_port, uint16_t mempool_port, uint16_t auth_port,
           uint16_t gateway_port, uint16_t metrics_port,
           std::optional<uint32_t> flag_shard_id,
           std::optional<uint32_t> flag_total_shards)
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
  StorageLongHash db_hash = db_tmp.compute_full_db_hash();

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
    CHECK_EQ(db_hash, StorageLongHash{});
    ConsensusValue value{};
    value.proposal_header.block_height = -1;
    ctx.set_tip(DbTip{.id = std::numeric_limits<BlockTXID>::max(),
                      .storage_long_hash = {},
                      .value = std::move(value)});
    Health::instance().set_current_block_height(-1)();
  }

  // Resolve shard identity: a persisted record is authoritative (flags only
  // confirm); with no record the flags seed it (default single-shard) and it is
  // persisted. Under a kGlobalMetadata key, so excluded from the consensus
  // hash.
  ShardIdentity shard_identity{};
  {
    HogDBStatus<ShardIdentity> persisted = ctx.get_db().read_shard_identity();
    if (persisted) {
      shard_identity = *persisted;
      if ((flag_shard_id && *flag_shard_id != shard_identity.shard_id) ||
          (flag_total_shards &&
           *flag_total_shards != shard_identity.total_shards)) {
        critical("shard identity mismatch: persisted {{id={}, total={}}} but "
                 "flags {{id={}, total={}}}; refusing to start",
                 shard_identity.shard_id, shard_identity.total_shards,
                 flag_shard_id ? std::to_string(*flag_shard_id) : "unset",
                 flag_total_shards ? std::to_string(*flag_total_shards)
                                   : "unset");
        return EXIT_FAILURE;
      }
      if (shard_identity.shard_function_id != kShardFunctionId) {
        critical("shard routing-function mismatch: persisted {} but this "
                 "binary uses {}; refusing to start",
                 shard_identity.shard_function_id, kShardFunctionId);
        return EXIT_FAILURE;
      }
    }
    else if (persisted.error().code() != HogDBCode::kNotFound) {
      // An unreadable identity record is not the same as "no record" -- do not
      // silently overwrite it.
      critical("failed to read shard identity: {}; refusing to start",
               persisted.error());
      return EXIT_FAILURE;
    }
    else {
      shard_identity =
          ShardIdentity{.shard_id = flag_shard_id.value_or(0),
                        .total_shards = flag_total_shards.value_or(1),
                        .shard_function_id = kShardFunctionId};
      // A db with committed state but no identity predates sharding, so it
      // holds the full single-shard dataset and can only be shard 0 of 1 --
      // reject flags that would relabel it. A fresh db may adopt any valid
      // identity.
      if (tip.has_value() &&
          (shard_identity.shard_id != 0 || shard_identity.total_shards != 1)) {
        critical("db has committed state but no shard identity (pre-sharding); "
                 "it can only be shard 0 of 1, not id={} of {}; refusing to "
                 "start",
                 shard_identity.shard_id, shard_identity.total_shards);
        return EXIT_FAILURE;
      }
      if (shard_identity.total_shards < 1 ||
          shard_identity.shard_id >= shard_identity.total_shards) {
        critical("invalid shard identity: id={}, total={}; require "
                 "total >= 1 and 0 <= id < total",
                 shard_identity.shard_id, shard_identity.total_shards);
        return EXIT_FAILURE;
      }
      if (auto w = ctx.get_db().write_shard_identity(shard_identity); !w) {
        critical("failed to persist shard identity: {}", w.error());
        return EXIT_FAILURE;
      }
      if (auto c = ctx.get_db().commit(); !c) {
        critical("failed to commit shard identity: {}", c.error());
        return EXIT_FAILURE;
      }
    }
  }
  info("shard identity: id={}, total_shards={}, function_id={}",
       shard_identity.shard_id, shard_identity.total_shards,
       shard_identity.shard_function_id);
  ctx.set_shard_identity(shard_identity);

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
  mempool_server.max_message_size_ = kBulkRpcMaxMessageSize;
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
                   .shard_id = shard_identity.shard_id,
                   .total_shards = shard_identity.total_shards,
                   .shard_function_id = shard_identity.shard_function_id,
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
               "[--metrics-port <port>] [--shard-id <n>] "
               "[--total-shards <n>] [--version]",
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
  std::optional<uint32_t> shard_id;
  std::optional<uint32_t> total_shards;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--shard-id" && i + 1 < argc) {
      shard_id = hogd::parse_integer_arg<uint32_t>("--shard-id", argv[++i],
                                                   argv, usage);
      continue;
    }
    if (arg == "--total-shards" && i + 1 < argc) {
      total_shards = hogd::parse_integer_arg<uint32_t>("--total-shards",
                                                       argv[++i], argv, usage);
      continue;
    }
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
                          metrics_port, shard_id, total_shards);
};
