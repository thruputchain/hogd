// hogdb.cc - handles initialization for hogdb

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <format>
#include <future>
#include <limits>
#include <memory>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <utility>

// XDR headers
#include "db_sched.hh"
#include "hogd/base/coropoll.h"
#include "sched_db.hh"

#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/rpc_context.h"
#include "hogd/hogdb/rpc_from_exec.h"
#include "hogd/hogdb/rpc_from_sched.h"
#include "hogd/hogdb/status.h"
#include "hogd/init.h"
#include "hogd/ramdb/db.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/version.h"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/sockio.h"

namespace hogd {

namespace {

// port numbers used by servers hosted inside hogdb.
constexpr uint16_t kExecServerPort = 8080;
constexpr uint16_t kSchedServerPort = 8081;

std::string
create_address(uint16_t port)
{
  std::string hostname;
  CHECK_EQ(get_hostname(hostname), 0);
  return std::format("{}:{}", hostname, port);
}

auto
create_sched_db_client(x23::CoroPoll &cp, const std::string &coordinator_addr)
{
  x23::HostPort host_port;
  if (!host_port.parse(coordinator_addr)) {
    throw std::runtime_error(
        std::format("Invalid coordinator address: {}", coordinator_addr));
  }
  auto addr = host_port.to_addr(true);
  if (!addr) {
    throw std::runtime_error(
        std::format("failed to resolve coordinator address: {}",
                    coordinator_addr));
  }

  auto fd = [&cp](sockaddr_storage addr) {
    while (true) {
      auto fd = cp.tcpconnect(&addr)();
      if (fd) {
        return *fd;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      warn_every_n_sec(5.0F, "waiting for coordinator address {}",
                       x23::show_ipaddr(addr));
    }
  }(*addr);

  return std::make_unique<
      SchedDb1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>>(
      x23::rpc_stream_logged(cp, fd)->template client<SchedDb1>());
}

int
hogdb_init(std::string_view path, size_t ram_bytes,
           const std::string *coordinator_addr)
{
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
  HogDBDAO db(RAMDB(std::move(*ramdb)));
  MerkleTriePrefix trie;
  db.load_trie(trie);

  std::unique_ptr<SchedDb1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>>
      sched_db_client;

  if (coordinator_addr) {
    sched_db_client =
        create_sched_db_client(default_coropoll(), *coordinator_addr);
  }
  else {
    warn("no coordinator address provided, skipping coordinator connection");
  }

  RPCContext ctx(db, trie, std::move(sched_db_client));

  // load the tip metadata from the database (won't exist in a new database).
  HogDBStatus<DbTip> tip = ctx.get_db().read_dbtip();
  if (tip) {
    if (trie.get_root_hash() != tip->hash) {
      critical("Root hash doesn't match DbTip: expected '{}', got '{}'",
               trie.get_root_hash(), tip->hash);
      return EXIT_FAILURE;
    }
    ctx.get_tip() = *tip;
    if (auto cost_model = db.read_cost_model(); cost_model) {
      ctx.initialize_cost_model(*cost_model);
    }
    else {
      critical("Failed to read cost model: {}", cost_model.error());
      return EXIT_FAILURE;
    }
  }
  else {
    CHECK_EQ(trie.size(), 0UL);
    ctx.get_tip().height = 0;
    ctx.get_tip().id = std::numeric_limits<BlockTXID>::max();
  }

  // start the executor server.
  info("starting executor server.");
  HogDBExecutorServer exec_server(ctx);
  exec_server.start_server(kExecServerPort);

  // start the scheduler server.
  info("starting scheduler server.");
  HogDBSchedServer sched_server(ctx);
  sched_server.start_server(kSchedServerPort);

  if (coordinator_addr) {
    auto const &client = ctx.get_sched_db_client();
    assert(client);

    const std::string exec_address = create_address(kExecServerPort);
    const std::string sched_address = create_address(kSchedServerPort);
    ShardCard card{.protocol = version::kHogdProtocol,
                   .upid = {},
                   .block_height = 0,
                   .address_for_exec = exec_address.c_str(),
                   .address_for_sched = sched_address.c_str()};

    client->scheddb_intro_shard(card)();
  }

  // block forever
  info("hogdb initialization complete.");
  std::promise<void>().get_future().wait();

  return EXIT_SUCCESS;
}

} // namespace

} // namespace hogd

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  int ret = hogd::hogd_init(argc, argv);
  if (ret)
    return ret;
  hogd::info("starting hogdb!");

  if (argc < 3 || argc >= 5) {
    std::println("usage: db_path ram_bytes [coordinator_ip:port]");
    return EXIT_FAILURE;
  }

  std::string path = argv[1];
  size_t ram_bytes = std::stoul(argv[2], nullptr, 0);

  const std::string coordinator_addr_raw = (argc < 4) ? "" : argv[3];
  const std::string *coordinator_addr =
      (argc < 4) ? nullptr : &coordinator_addr_raw;

  return hogd::hogdb_init(path, ram_bytes, coordinator_addr);
};
