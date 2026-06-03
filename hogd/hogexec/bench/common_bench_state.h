#pragma once

#include <atomic>
#include <cassert>
#include <format>
#include <memory>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "db_sched.hh"
#include "event_rpc.hh"
#include "exec_result.hh"
#include "genesis.hh"
#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hogexec/bench/control_client.h"
#include "hogd/hogexec/bench/mock_scheduler.h"
#include "hogd/hogexec/components/execution_mode.h"
#include "hogd/hogexec/precompiles/context.h"
#include "hogd/hogexec/runtime/event_server_proxy.h"
#include "hogd/hogexec/runtime/item/database_proxy.h"
#include "hogd/hogexec/runtime/item/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "loadgen.hh"
#include "rpc_types.hh"
#include "sched_db.hh"
#include "sched_event.hh"
#include "sched_exec.hh"
#include "transaction.hh"
#include "x23/rpc.h"
#include "x23/sockio.h"
#include "x23/srpc.h"
#include "x23/vec.h"

namespace hogd {

inline std::string
hog_ip(std::string_view host)
{
  static const std::regex kHogHostPattern("^hog([0-9]+)$");
  std::match_results<std::string_view::const_iterator> match;
  if (!std::regex_match(host.begin(), host.end(), match, kHogHostPattern)) {
    throw std::runtime_error(std::format("invalid hog host: {}", host));
  }

  unsigned int hog_num = std::stoul(match[1].str());
  return std::format("10.100.0.{}", 100 + hog_num);
}

class CommonBenchState {
public:
  CommonBenchState(std::vector<std::string> server_addrs, uint16_t exec_port,
                   uint16_t coord_port)
    : server_addrs_(std::move(server_addrs)), exec_port_(exec_port),
      coord_port_(coord_port)
  {
    CHECK_EQ(server_addrs_.size(), 2U);

    hogdb_handle_ = std::make_unique<ControlServerHandle>(hogdb_addr());
    event_server_handle_ =
        std::make_unique<ControlServerHandle>(event_server_addr());

    uint16_t event_server_port = mock_sched_event_server_.start_server(0);

    start_db_server();
    event_card_ = start_event_server(event_server_port);
    connect_coord();
    connect_exec();
    connect_event();

    connect_event_sched();
  }

  // NOLINTBEGIN(bugprone-exception-escape)
  ~CommonBenchState() { close_event_block(block_number_); }
  // NOLINTEND(bugprone-exception-escape)

  SystemCallHandler make_handler(SignedTransaction const &signed_tx,
                                 TransactionID txid,
                                 RuntimeContext &runtime_context)
  {
    auto proxy = PerTransactionDatabaseProxy(*db_proxy_, block_number_, txid);
    auto event_proxy = PerTransactionEventServerProxy(*event_server_proxy_,
                                                      block_number_, txid);
    return SystemCallHandler(signed_tx.tx, *snapshot_cache_, std::move(proxy),
                             std::move(event_proxy), ProposalModeCtor{},
                             runtime_context);
  }

  void advance_block();

  void run_genesis_configuration(
      GenesisConfigurationBuilder const &genesis_configuration_builder);
  void run_genesis_configuration(
      GenesisConfiguration const &genesis_configuration,
      x23::XdrVec<InitializationBlock> const &initialization_txs);

  TransactionID allocate_txid()
  {
    return next_tx_id_.fetch_add(1, std::memory_order_relaxed);
  }

  // Height of the block transactions currently execute against. Used to derive
  // uniquifier expiry heights that are not yet expired (expiry < height is
  // rejected at commit).
  BlockID block_height() const { return block_number_; }

  // Replay-cache-safe uniquifier assignment for benchmarks.
  //
  // HogDB enforces unique Uniquifier consumption per invoked address, with at
  // most kReplayCacheNoncesMaxLen (8000) nonces per expiry-height and
  // kReplayCacheHeightsMaxLen (4000) distinct unexpired expiry-heights per
  // address. A bench commits far more than that to a handful of addresses, so
  // it must (a) keep each expiry-height under the per-height cap and (b) expire
  // old uniquifiers between reps.
  //
  // bench_uniquifier buckets a rep-local `index` into groups of
  // kBenchUniquifiersPerHeight, giving at most that many distinct nonces per
  // expiry-height across all addresses (comfortably under 8000). Expiry heights
  // start at the rep's base height, so none are pre-expired at commit time.
  static constexpr uint64_t kBenchUniquifiersPerHeight = 4000;

  static Uniquifier bench_uniquifier(BlockID base_height, uint64_t index)
  {
    auto expiry = static_cast<BlockHeight>(
        base_height + (index / kBenchUniquifiersPerHeight));
    return Uniquifier{.block_expiry_height = expiry, .nonce = index};
  }

  // Advance past (and thereby prune) every uniquifier bench_uniquifier would
  // assign for a rep of `num_txs` transactions started at the current height.
  // Must be called outside any timed region.
  void clear_uniquifiers(uint64_t num_txs)
  {
    if (num_txs == 0) {
      return;
    }
    uint64_t max_offset = (num_txs - 1) / kBenchUniquifiersPerHeight;
    for (uint64_t i = 0; i <= max_offset; i++) {
      advance_block();
    }
  }

private:
  void connect_coord()
  {
    coord_client_ = std::make_unique<coord_client_t>(
        make_srpc_client<DbSched1>(x23::HostPort{hogdb_addr(), coord_port_},
                                   true));
  }

  void connect_event_sched()
  {
    x23::HostPort host_port;
    CHECK(host_port.parse(event_card_.address_for_sched));
    event_sched_client_ = std::make_unique<event_sched_client_t>(
        make_srpc_client<EventSched1>(host_port, true));

    x23::HostPort exec_host_port;
    CHECK(exec_host_port.parse(event_card_.address_for_exec));
    event_exec_client_ = std::make_unique<event_exec_client_t>(
        make_srpc_client<EventExecutor1>(exec_host_port, true));
  }

  void connect_event()
  {
    x23::HostPort host_port;
    CHECK(host_port.parse(event_card_.address_for_exec));
    event_server_proxy_ = std::make_unique<EventServerProxy>(host_port);
  }

  void connect_exec()
  {
    std::string server_addrport =
        hogdb_addr() + ":" + std::to_string(exec_port_);
    db_proxy_ = std::make_unique<DatabaseProxy>(ShardedDB(IntroExecResult{
        .exec_id = 0,
        .shards = {ShardCard{.address_for_exec = server_addrport.c_str()}}}));
  }

  void start_db_server()
  {
    CHECK_PTR(hogdb_handle_.get());

    hogdb_handle_->ci_client.start_server("hogd/hogexec/bench/bench_server",
                                          {"-e", std::to_string(exec_port_),
                                           "-c", std::to_string(coord_port_)});
  }

  EventCard start_event_server(uint16_t event_server_port)
  {
    std::string hostname;
    CHECK_EQ(get_hostname(hostname), 0);
    std::string mempool_ip = hog_ip(hostname);

    auto work_dir = event_server_handle_->ci_client.query_work_dir();

    event_server_handle_->ci_client.start_server(
        "hogd/event/event_exec",
        {"--mempool-addr", std::format("{}:{}", mempool_ip, event_server_port),
         "--bind-ip", hog_ip(event_server_addr()), "--work-dir",
         std::string(work_dir.work_dir().begin(), work_dir.work_dir().end())});

    return mock_sched_event_server_->impl()->await_event_card();
  }

  void open_event_block(BlockID block_number)
  {
    auto r = event_sched_client_->eventsched_open_block(block_number);
    if (r != RPCError::kRPCNone) {
      throw std::runtime_error("Failed to open event block");
    }
    auto r2 = event_exec_client_->eventexecutor_open_block(block_number);
    if (r2 != RPCError::kRPCNone) {
      throw std::runtime_error("Failed to open executor event handle");
    }
  }
  void close_event_block(BlockID block_number)
  {
    // Close executor handle first so the event server knows all events
    // have been emitted before it finalizes the block.
    auto r1 = event_exec_client_->eventexecutor_close_block(block_number);
    if (r1 != RPCError::kRPCNone) {
      throw std::runtime_error("Failed to close executor event handle");
    }
    // We use [block_number] as the height as well.
    auto r =
        event_sched_client_->eventsched_close_block(block_number, block_number);
    if (r.error() != RPCError::kRPCNone) {
      throw std::runtime_error("Failed to close event block");
    }
  }

  std::string hogdb_addr() const { return server_addrs_[0]; }
  std::string event_server_addr() const { return server_addrs_[1]; }

  std::vector<std::string> server_addrs_;

  std::unique_ptr<ControlServerHandle> hogdb_handle_;
  std::unique_ptr<ControlServerHandle> event_server_handle_;

  EventCard event_card_;

  uint16_t exec_port_;
  uint16_t coord_port_;

  std::atomic<TransactionID> next_tx_id_ = 0;
  BlockID block_number_ = 0;

  std::unique_ptr<DatabaseProxy> db_proxy_;
  std::unique_ptr<StorageSnapshotCache> snapshot_cache_;
  std::unique_ptr<EventServerProxy> event_server_proxy_;

  RpcServer<MockSchedEvent> mock_sched_event_server_;

  using coord_client_t =
      DbSched1::client<x23::SRpcStream<x23::RpcLogger>::ClientRef>;
  using event_sched_client_t =
      EventSched1::client<x23::SRpcStream<x23::RpcLogger>::ClientRef>;
  using event_exec_client_t =
      EventExecutor1::client<x23::SRpcStream<x23::RpcLogger>::ClientRef>;

  std::unique_ptr<coord_client_t> coord_client_;
  std::unique_ptr<event_sched_client_t> event_sched_client_;
  std::unique_ptr<event_exec_client_t> event_exec_client_;
};

inline void
CommonBenchState::advance_block()
{
  auto r = coord_client_->dbsched_close_snapshot(block_number_);
  if (r.result() != DbSchedResult::DbSchedResultOK) {
    throw std::runtime_error("Failed to close snapshot");
  }

  close_event_block(block_number_);

  auto r2 =
      // NOLINTBEGIN(bugprone-argument-comment)
      coord_client_->dbsched_commit_snapshot(block_number_, /*block_hash=*/{},
                                             /*events_hash=*/{});
  // NOLINTEND(bugprone-argument-comment)
  if (r2.result() != DbSchedResult::DbSchedResultOK) {
    throw std::runtime_error("Failed to commit snapshot");
  }

  auto r3 = coord_client_->dbsched_create_snapshot(
      block_number_, block_number_ + 1,
      BlockExecutionMode::kBlockExecutionModeProposal);
  if (r3 != DbSchedResult::DbSchedResultOK) {
    throw std::runtime_error("Failed to create snapshot");
  }

  open_event_block(block_number_ + 1);

  block_number_++;

  snapshot_cache_ =
      std::make_unique<StorageSnapshotCache>(block_number_, *db_proxy_);
}

// leaves the benchmark in a state where it's ready to execute transactions
inline void
CommonBenchState::run_genesis_configuration(
    GenesisConfigurationBuilder const &genesis_configuration_builder)
{
  auto unfurl_txs =
      genesis_configuration_builder.make_unfurl_genesis_transactions();
  InitializationBlock initialization_txs = {unfurl_txs.begin(),
                                            unfurl_txs.end()};
  run_genesis_configuration(genesis_configuration_builder.build(
                                StorageCostModel::default_cost_model()),
                            {initialization_txs});
}

inline void
CommonBenchState::run_genesis_configuration(
    GenesisConfiguration const &genesis_configuration,
    x23::XdrVec<InitializationBlock> const &initialization_txs)
{

  assert(block_number_ == 0);
  {
    uint64_t network_id = 0x0;
    SHA256Hash txset_hash{};
    SHA256Hash events_hash{};
    auto r = coord_client_->dbsched_load_genesis_configuration(
        genesis_configuration, network_id, txset_hash, events_hash);
    if (r != DbSchedResult::DbSchedResultOK) {
      throw std::runtime_error("Failed to write genesis configuration");
    }
  }

  {
    auto r = coord_client_->dbsched_create_snapshot(
        0, 1, BlockExecutionMode::kBlockExecutionModeProposal);
    if (r != DbSchedResult::DbSchedResultOK) {
      throw std::runtime_error("Failed to create snapshot");
    }
  }

  block_number_++;
  snapshot_cache_ =
      std::make_unique<StorageSnapshotCache>(block_number_, *db_proxy_);

  open_event_block(block_number_);

  auto runtime_context = make_hogexec_context();

  for (auto const &block : initialization_txs) {
    for (auto const &tx : block) {
      auto handler = make_handler(tx, allocate_txid(), *runtime_context);
      if (!handler.root_invoke().is_root_success()) {
        throw std::runtime_error("Failed to invoke transaction");
      }
      if (handler.try_commit_transaction() != ExecResult::ExecResultOK) {
        throw std::runtime_error("failed to perform commit operation");
      }
    }
    advance_block();
  }
}

} // namespace hogd
