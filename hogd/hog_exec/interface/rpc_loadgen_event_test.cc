#include <gtest/gtest.h>

#include <format>
#include <memory>
#include <set>
#include <utility>

#include "consensus.hh"
#include "db_sched.hh"
#include "event_rpc.hh"
#include "exec_result.hh"
#include "exec_sched.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/os.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_exec/interface/rpc_from_coord.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/hog_exec/runtime/test_util/hog_db_wrapper.h"
#include "hogd/hog_mempool/shared.h"
#include "hogd/hog_mempool/config_hash.h"
#include "hogd/loadgen/loadgen_server.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/version.h"
#include "loadgen.hh"
#include "rpc_types.hh"
#include "sched_db.hh"
#include "sched_event.hh"
#include "sched_exec.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/cleanup.h"
#include "x23/refcnt.h"
#include "x23/rpc.h"
#include "x23/sockio.h"
#include "x23/srpc.h"
#include "x23/vec.h"

namespace hogd {

namespace {

class RpcLoadgenEventTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    // Start loadgen RPC server on a random port
    loadgen_port_ = loadgen_server_.start_server(0);

    // Start executor RPC server on a random port
    executor_port_ = rpc_server_.start_server(0);

    // Create executor RPC client connected to the server
    client_ = make_srpc_multiclient<ExecutorSched1>(
        x23::HostPort{"localhost", executor_port_}, false);

    db_client_ = make_srpc_multiclient<DbSched1>(
        x23::HostPort{"localhost", db_wrapper_.coord_port()}, false);

    // Create loadgen client
    x23::HostPort loadgen_host_port{"localhost", loadgen_port_};
    loadgen_client_ = std::make_unique<loadgen_client_t>(
        create_client<LoadgenInterfaceV1>(loadgen_host_port, false,
                                          LoadgenRpc::kRpcMaxMessageSize)()
            .value());

    event_client_ = make_srpc_multiclient<EventSched1>(
        x23::HostPort{"localhost", event_wrapper_.sched_port()}, false);
  }

  HogdbWrapper db_wrapper_;
  EventServerWrapper event_wrapper_;

  RpcServer<LoadgenRpc> loadgen_server_{default_coropoll()};
  uint16_t loadgen_port_ = 0;

  RpcServer<ExecutorMempoolRpc> rpc_server_{default_coropoll()};
  uint16_t executor_port_ = 0;

  ExecutorSched1::client<x23::RefPtr<x23::SRpcMulticlient<x23::RpcLogger>>>
      client_;
  DbSched1::client<x23::RefPtr<x23::SRpcMulticlient<x23::RpcLogger>>>
      db_client_;
  std::unique_ptr<loadgen_client_t> loadgen_client_;

  EventSched1::client<x23::RefPtr<x23::SRpcMulticlient<x23::RpcLogger>>>
      event_client_;

  x23::Defer<> force_syscallhandler_commit_ =
      SystemCallHandler::force_commit_or_abort();

  ShardCard make_shard_card()
  {
    return ShardCard{
        .protocol = hogd::version::kHogdProtocol(),
        .upid = {},
        .block_height = 0,
        .address_for_exec =
            std::format("localhost:{}", db_wrapper_.exec_port()).c_str(),
        .address_for_sched =
            std::format("localhost:{}", db_wrapper_.coord_port()).c_str()};
  }
  EventCard make_event_card() { return event_wrapper_.make_event_card(); }

  void open_block(BlockID block_id, BlockID parent_block_id)
  {
    auto open_res = client_.executorsched_open_proposal_block(block_id);
    ASSERT_EQ(open_res, ExecResult::ExecResultOK);

    auto db_res = db_client_.dbsched_create_snapshot(
        parent_block_id, block_id,
        BlockExecutionMode::kBlockExecutionModeProposal);
    ASSERT_EQ(db_res, DbSchedResult::DbSchedResultOK);

    auto event_open_res = event_client_.eventsched_open_block(block_id);
    ASSERT_EQ(event_open_res, RPCError::kRPCNone);
  }

  void close_block(BlockID block_id)
  {
    auto client_finish_res = client_.executorsched_finish_block(block_id);
    ASSERT_EQ(client_finish_res, ExecResult::ExecResultOK);

    // We use [block_id] as the height as well.
    auto event_close_res =
        event_client_.eventsched_close_block(block_id, block_id);
    ASSERT_EQ(event_close_res.error(), RPCError::kRPCNone);

    auto db_close_res = db_client_.dbsched_close_snapshot(block_id);
    ASSERT_EQ(db_close_res.result(), DbSchedResult::DbSchedResultOK);

    ConsensusValue value{};
    value.proposal_header.block_height = block_id;
    auto db_commit_res = db_client_.dbsched_commit_snapshot(block_id, value);
    ASSERT_EQ(db_commit_res.result(), DbSchedResult::DbSchedResultOK);
  }
};

//
// Test that loadgen ERC20 payment transactions emit events.
//
TEST_F(RpcLoadgenEventTest, TestLoadgenPaymentsEmitEvents)
{
  BlockID test_block_id = 100;

  // Generate an experiment with ERC20 payments
  constexpr uint32_t kNumAddresses = 5;
  ExperimentConfig config(ExperimentType::kExperimentTypeERC20Payments);
  config.erc20_payments().num_addresses = kNumAddresses;

  LoadgenResult result = loadgen_client_->generate_experiment(config)();
  ASSERT_EQ(result.error(), LoadgenRPCError::kLoadgenRPCErrorOK);
  LoadgenContextID context_id = result.success().context_id;

  // Load the genesis configuration from loadgen to the database
  ConsensusValue genesis_value{};
  genesis_value.proposal_header.block_height = 0;
  genesis_value.proposal_header.config_hash = compute_config_hash(
      result.success().genesis_configuration.storage_cost_model);
  auto db_res = db_client_.dbsched_load_genesis_configuration(
      result.success().genesis_configuration, genesis_value);
  ASSERT_EQ(db_res, DbSchedResult::DbSchedResultOK);

  // Initialize the executor with a shard pointing to our mock database server
  IntroExecResult exec_result{
      .exec_id = 1,
      .shards = x23::Vec<ShardCard>{make_shard_card()},
      .event = make_event_card(),
  };
  rpc_server_->impl()->init(std::move(exec_result), available_parallelism());

  // Run initialization transactions
  BlockID current_block_id = 1;

  auto run_initialization_block = [&](const InitializationBlock &init_block) {
    open_block(current_block_id, current_block_id - 1);

    // Propose all transactions in this initialization block
    for (const auto &stx : init_block) {
      auto propose_res =
          client_.executorsched_propose_transaction(current_block_id, stx);
      ASSERT_NE(propose_res.result(), ExecResult::ExecResultSystemError);
    }

    // Finish the initialization block
    close_block(current_block_id);
    current_block_id++;
  };

  run_initialization_block(result.success().genesis_ceremony_txs);
  for (const InitializationBlock &init_block :
       result.success().initialization_txs) {
    run_initialization_block(init_block);
  }

  // Now open the test block for work transactions
  open_block(test_block_id, current_block_id - 1);

  // Get some payment transactions from loadgen
  constexpr size_t kNumTxs = 5;
  LoadgenExtensionResult ext_result =
      loadgen_client_->extend_experiment(context_id, kNumTxs, 1'000'000'000)();
  ASSERT_EQ(ext_result.error(), LoadgenRPCError::kLoadgenRPCErrorOK);

  SignedTransactionBatch txs;
  parallel_batch_to_t(ext_result.success().work_txs, txs)();
  ASSERT_EQ(txs.size(), kNumTxs);

  // Verify all transaction hashes are unique
  std::set<SHA256Hash> tx_hashes;
  for (const auto &tx : txs) {
    ASSERT_TRUE(tx_hashes.insert(sha256(tx.tx)).second)
        << "Duplicate transaction hash detected";
  }

  // Propose each transaction
  for (const auto &stx : txs) {
    auto propose_res =
        client_.executorsched_propose_transaction(test_block_id, stx);
    // Transactions might succeed or fail depending on account balances, etc.
    // But they should not return SystemError if everything is set up correctly
    ASSERT_NE(propose_res.result(), ExecResult::ExecResultSystemError);
  }

  // Finish the block
  close_block(test_block_id);
  // Now all the events should be emitted

  // Look up events by transaction hash
  for (const auto &tx : txs) {
    SHA256Hash tx_hash = sha256(tx.tx);
    auto events = event_wrapper_.get_events_by_hash(test_block_id, tx_hash)();
    if (!events.has_value()) {
      ADD_FAILURE() << "Events should be present";
      return;
    }
    ASSERT_GT(events.value().events.size(), 0)
        << "Events should have at least one event";
  }
}

//
// Test that event emission can be delayed and then unblocked.
//
TEST_F(RpcLoadgenEventTest, TestEventEmissionDelay)
{
  BlockID test_block_id = 200;

  // Generate an experiment with ERC20 payments
  constexpr uint32_t kNumAddresses = 5;
  ExperimentConfig config(ExperimentType::kExperimentTypeERC20Payments);
  config.erc20_payments().num_addresses = kNumAddresses;

  LoadgenResult result = loadgen_client_->generate_experiment(config)();
  ASSERT_EQ(result.error(), LoadgenRPCError::kLoadgenRPCErrorOK);
  LoadgenContextID context_id = result.success().context_id;

  // Load the genesis configuration from loadgen to the database
  ConsensusValue genesis_value{};
  genesis_value.proposal_header.block_height = 0;
  genesis_value.proposal_header.config_hash = compute_config_hash(
      result.success().genesis_configuration.storage_cost_model);
  auto db_res = db_client_.dbsched_load_genesis_configuration(
      result.success().genesis_configuration, genesis_value);
  ASSERT_EQ(db_res, DbSchedResult::DbSchedResultOK);

  // Initialize the executor
  IntroExecResult exec_result{
      .exec_id = 1,
      .shards = x23::Vec<ShardCard>{make_shard_card()},
      .event = make_event_card(),
  };
  rpc_server_->impl()->init(std::move(exec_result), available_parallelism());

  // Run initialization transactions
  BlockID current_block_id = 1;

  auto run_initialization_block = [&](const InitializationBlock &init_block) {
    open_block(current_block_id, current_block_id - 1);

    // Propose all transactions in this initialization block
    for (const auto &stx : init_block) {
      auto propose_res =
          client_.executorsched_propose_transaction(current_block_id, stx);
      ASSERT_EQ(propose_res.result(), ExecResult::ExecResultOK);
    }

    // Finish the initialization block
    close_block(current_block_id);
    current_block_id++;
  };

  run_initialization_block(result.success().genesis_ceremony_txs);
  for (const InitializationBlock &init_block :
       result.success().initialization_txs) {
    run_initialization_block(init_block);
  }

  // Now open the test block for work transactions
  open_block(test_block_id, current_block_id - 1);

  // Block event emission before proposing transactions
  event_wrapper_.block_emit_events();

  // Get payment transactions from loadgen
  constexpr size_t kNumTxs = 3;
  LoadgenExtensionResult ext_result =
      loadgen_client_->extend_experiment(context_id, kNumTxs, 1'000'000'000)();
  ASSERT_EQ(ext_result.error(), LoadgenRPCError::kLoadgenRPCErrorOK);

  SignedTransactionBatch txs;
  parallel_batch_to_t(ext_result.success().work_txs, txs)();
  ASSERT_EQ(txs.size(), kNumTxs);

  // Verify all transaction hashes are unique
  std::set<SHA256Hash> tx_hashes;
  for (const auto &tx : txs) {
    ASSERT_TRUE(tx_hashes.insert(sha256(tx.tx)).second)
        << "Duplicate transaction hash detected";
  }

  // Propose transactions while events are blocked
  for (const auto &stx : txs) {
    auto propose_res =
        client_.executorsched_propose_transaction(test_block_id, stx);
    ASSERT_NE(propose_res.result(), ExecResult::ExecResultSystemError);
  }

  // Verify events haven't been emitted yet (they're blocked)
  for (const auto &tx : txs) {
    SHA256Hash tx_hash = sha256(tx.tx);
    auto events = event_wrapper_.get_events_by_hash(test_block_id, tx_hash)();
    // Events should not be present yet since emission is blocked
    ASSERT_FALSE(events.has_value()) << "Events should be blocked";
  }

  // Unblock event emission
  event_wrapper_.unblock_emit_events();

  // Finish the block
  close_block(test_block_id);

  // Check that events were emitted using hash-based lookup
  bool found_events = false;
  for (const auto &tx : txs) {
    SHA256Hash tx_hash = sha256(tx.tx);
    auto events = event_wrapper_.get_events_by_hash(test_block_id, tx_hash)();
    if (events.has_value() && !events.value().events.empty()) {
      found_events = true;
      break;
    }
  }
  // At least one transaction should have emitted events after unblocking
  ASSERT_TRUE(found_events) << "No events were emitted after unblocking";
}

} // namespace

} // namespace hogd
