#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <mutex>
#include <optional>
#include <print>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

#include "db_sched.hh"
#include "event_rpc.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/bench/control_client.h"
#include "hogd/hogexec/runtime/test_util/test_event_aggregator.h"
#include "hogd/init.h"
#include "hogd/loadgen/loadgen_server.h"
#include "hogd/version.h"
#include "loadgen.hh"
#include "rpc_types.hh"
#include "sched_db.hh"
#include "sched_event.hh"
#include "sched_exec.hh"
#include "x23/async.h"
#include "x23/format.h"
#include "x23/sockio.h"
#include "x23/srpc.h"
#include "x23/vec.h"

namespace hogd {

class ControlServerHandle {
public:
  ControlServerHandle(std::string const &server_addr)
    : ci_client(connect_to_control_server(default_coropoll(), server_addr,
                                          std::to_string(kControlServerPort)))
  {}

  ControlClientWrapper ci_client;

  ~ControlServerHandle() { ci_client.stop_server(); }
};

class SchedExecTest {
public:
  using rpc_service = SchedExec1;

  SchedExecTest() = default;

  // Disable copy and move.
  SchedExecTest(const SchedExecTest &) = delete;
  SchedExecTest &operator=(const SchedExecTest &) = delete;
  SchedExecTest(SchedExecTest &&) = delete;
  SchedExecTest &operator=(SchedExecTest &&) = delete;

  x23::Async<IntroExecResult> schedexec_intro_exec(const ExecCard& card)
  {
    info("executor with card {} connected", x23::show(card));

    auto r = default_coropoll().pool_.async([this, &card]() {

        std::unique_lock lk(mtx_);
        exec_cards_.push_back(card);

        cv_.wait(lk, [this] { return event_card_ != EventCard{}; });
        cv_.wait(lk, [this] { return !shard_cards_.empty(); });

        return IntroExecResult{.exec_id = exec_id_++,
            .shards = shard_cards_,
            .event = event_card_};
    });

    co_return co_await r;
  }

  void register_event_card(EventCard card)
  {
    std::unique_lock lk(mtx_);
    event_card_ = std::move(card);
    cv_.notify_all();
  }

  void register_shard_cards(x23::Vec<ShardCard> cards)
  {
    std::unique_lock lk(mtx_);
    shard_cards_ = std::move(cards);
    cv_.notify_all();
  }

  std::vector<ExecCard> await_executor_cards(int num_cards)
  {
    std::unique_lock lk(mtx_);
    cv_.wait(lk, [this, num_cards] { return exec_cards_.size() == num_cards; });
    return std::move(exec_cards_);
  }

  void shut_down()
  {}

private:
    std::mutex mtx_;
    std::condition_variable cv_;
    std::vector<ExecCard> exec_cards_;
    uint16_t exec_id_ = 0;
    EventCard event_card_{};
    x23::Vec<ShardCard> shard_cards_;
};

static_assert(is_SchedExec1<SchedExecTest *, x23::MaybeAsync>);

class SchedDBTest {
public:
  using rpc_service = SchedDb1;

  SchedDBTest() = default;

  // Disable copy and move.
  SchedDBTest(const SchedDBTest &) = delete;
  SchedDBTest &operator=(const SchedDBTest &) = delete;
  SchedDBTest(SchedDBTest &&) = delete;
  SchedDBTest &operator=(SchedDBTest &&) = delete;

  void scheddb_intro_shard(const ShardCard &card)
  {
    std::unique_lock lk(mtx_);
    shard_cards_.push_back(card);
    cv_.notify_all();
  }

  static void scheddb_hogs_transferred(const HogsTransferred &hogs)
  {

  }

  std::vector<ShardCard> await_shard_cards(int num_cards)
  {
    std::unique_lock lk(mtx_);
    cv_.wait(lk, [this, num_cards] { return shard_cards_.size() == num_cards; });
    return shard_cards_;
  }

private:
  std::mutex mtx_;
  std::condition_variable cv_;
  std::vector<ShardCard> shard_cards_;
};

static_assert(is_SchedDb1<SchedDBTest *, std::type_identity_t>);

namespace {
auto connect_to_event_sched_server(uint16_t port)
{
    auto server_addr = x23::HostPort{"localhost", port}.to_addr(true);
    if (!server_addr) {
    throw std::runtime_error("Failed to connect to event server");
    }
    return x23::SRpcMulticlient<x23::RpcLogger>::make(server_addr.value())
        ->client(EventSched1{});
}

auto connect_to_db_server(ShardCard const& card) {
    x23::HostPort host_port;
    if (!host_port.parse(card.address_for_sched)) {
        throw std::runtime_error("Failed to parse db server address");
    }
    auto server_addr = host_port.to_addr(true);
    if (!server_addr) {
        throw std::runtime_error("Failed to connect to db server");
    }
    return x23::SRpcMulticlient<x23::RpcLogger>::make(server_addr.value())
        ->client(DbSched1{});
}
}

// this assumes the executors use the same port config
void
run_multiexec_bench(const std::vector<std::string> &server_addrs, unsigned int num_accounts)
{
  std::vector<ControlServerHandle> control_server_handles;
  control_server_handles.reserve(server_addrs.size());
  for (const auto &server_addr : server_addrs) {
    control_server_handles.emplace_back(server_addr);
  }

  if (server_addrs.size() < 2) {
    // 1 db and the rest executors
    throw std::runtime_error("At least 2 servers are required");
  }

  RpcServer<SchedExecTest> sched_exec_test;
  uint16_t sched_exec_port = sched_exec_test.start_server(0);

  RpcServer<SchedDBTest> sched_db_test;
  uint16_t sched_db_port = sched_db_test.start_server(0);

  TestEventAggregator event_server;
  TestEventAggregatorExecServer event_server_exec_server =
      TestEventAggregatorExecServer(event_server);
  TestEventAggregatorSchedServer event_server_sched_server =
      TestEventAggregatorSchedServer(event_server);

  uint16_t event_server_exec_port = event_server_exec_server.start_server(0);
  uint16_t event_server_sched_port = event_server_sched_server.start_server(0);

  std::string hostname;
  CHECK_EQ(get_hostname(hostname), 0);

  sched_exec_test.impl_.register_event_card(EventCard {.protocol = version::kHogdProtocol,
                                                       .upid = {},
                                                       .address_for_exec = std::format("{}:{}", hostname, event_server_exec_port).c_str(),
                                                       .address_for_sched = std::format("{}:{}", hostname, event_server_sched_port).c_str()});

  size_t server_idx = 0;
  control_server_handles[server_idx].ci_client.start_server(
      "hogd/hogdb/hogdb",
      {"hogdb_data/", "10000000000", std::format("{}:{}", hostname, sched_db_port)});
  server_idx++;

  for (; server_idx < server_addrs.size(); server_idx++) {
    control_server_handles[server_idx].ci_client.start_server(
        "hogd/hogexec/hogexec",
        {std::format("{}:{}", hostname, sched_exec_port)});
  }

  auto db_cards = sched_db_test.impl_.await_shard_cards(1);
  auto exec_cards = sched_exec_test.impl_.await_executor_cards(server_addrs.size() - 1);

  auto event_sched_client = connect_to_event_sched_server(event_server_sched_port);
  assert(db_cards.size() == 1);
  auto db_sched_client = connect_to_db_server(db_cards[0]);

  auto open_block = [&](uint64_t parent_block_number, uint64_t block_number) {
    event_sched_client.eventsched_open_block(block_number);
    auto res = db_sched_client.dbsched_create_snapshot(parent_block_number, block_number, BlockExecutionMode::kBlockExecutionModeProposal);
    if (res != DbSchedResult::DbSchedResultOK) {
      throw std::runtime_error("Failed to create snapshot");
    }
  };

  auto close_block = [&](uint64_t block_number) {
    event_sched_client.eventsched_close_block(block_number);
    auto res = db_sched_client.dbsched_close_snapshot(block_number);
    if (res.result() != DbSchedResult::DbSchedResultOK) {
      throw std::runtime_error("Failed to close snapshot");
    }
    auto commit_res = db_sched_client.dbsched_commit_snapshot(block_number);
    if (commit_res.result() != DbSchedResult::DbSchedResultOK) {
      throw std::runtime_error("Failed to commit snapshot");
    }
  };

  auto write_genesis_block = [&](const GenesisBlock &genesis_block) {
    auto res = db_sched_client.dbsched_load_genesis_block(genesis_block);
    if (res != DbSchedResult::DbSchedResultOK) {
      throw std::runtime_error("Failed to write genesis block");
    }
  };

  uint64_t parent_block_number = 0;
  uint64_t current_block_number = 1;

  ExperimentConfig config(ExperimentType::kExperimentTypeERC20Payments);
  config.erc20_payments() = {
    .num_accounts = num_accounts,
  };

  LoadgenRpc loadgen;
  auto loadgen_result = loadgen.generate_experiment(config);
  if (loadgen_result.error() != LoadgenRPCError::kLoadgenRPCErrorOK) {
    throw std::runtime_error("Failed to generate loadgen experiment");
  }
  auto genesis_block = loadgen_result.success().genesis_block;
  write_genesis_block(genesis_block);
  
  for (size_t i = 0; i < 10; i++) {
    open_block(parent_block_number, current_block_number);
    // TODO do the work of executing transactions

    close_block(current_block_number);
    parent_block_number = current_block_number;
    current_block_number++;
  }
}

} // namespace hogd

namespace {
void
usage(char *argv[])
{
  std::println(
      "Usage: {} -s <server_addr, repeated> -a "
      "<num_accounts>",
      argv[0]);
  exit(EXIT_FAILURE);
}
} // namespace
int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  auto ret = hogd::hogd_init(argc, argv);
  if (ret)
    return ret;

  std::vector<std::string> server_addrs;
  std::optional<int> num_accounts;

  int opt;
  // NOLINTBEGIN(misc-include-cleaner)
  while ((opt = getopt(argc, argv, "s:a:")) != -1) {
    switch (opt) {
    case 's':
      server_addrs.emplace_back(optarg);
      break;
    case 'a':
      num_accounts = std::stoi(optarg);
      break;
    default:
      usage(argv);
    }
  }
  // NOLINTEND(misc-include-cleaner)
  if (server_addrs.empty() || !num_accounts) {
    usage(argv);
  }

  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  hogd::run_multiexec_bench(server_addrs, *num_accounts);
  // NOLINTEND(bugprone-unchecked-optional-access)

  std::fflush(stdout);
  return 0;
}
