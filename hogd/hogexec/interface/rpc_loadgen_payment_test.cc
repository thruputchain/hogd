#include <gtest/gtest.h>

#include <format>
#include <memory>
#include <utility>

#include "db_sched.hh"
#include "exec_result.hh"
#include "exec_sched.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/os.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/interface/rpc_from_coord.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/test_util/hogdb_wrapper.h"
#include "hogd/loadgen/loadgen_server.h"
#include "hogd/mempool/shared.h"
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

class RpcLoadgenPaymentTest : public ::testing::Test {
public:
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
                                          LoadgenRpc::kRpcMaxMessageSize)());
  }

  // void TearDown() override { rpc_server_.shut_down(); }

protected:
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
};

//
// Test that we can generate payment transactions from loadgen and propose them.
//
TEST_F(RpcLoadgenPaymentTest, TestProposeLoadgenPayments)
{
  BlockID test_block_id = 100;

  // Generate an experiment with ERC20 payments
  constexpr uint32_t kNumAddresses = 10;
  ExperimentConfig config(ExperimentType::kExperimentTypeERC20Payments);
  config.erc20_payments().num_addresses = kNumAddresses;

  LoadgenResult result = loadgen_client_->generate_experiment(config)();
  ASSERT_EQ(result.error(), LoadgenRPCError::kLoadgenRPCErrorOK);
  LoadgenContextID context_id = result.success().context_id;

  // Load the genesis configuration from loadgen to the database
  uint64_t network_id = 0x0;
  SHA256Hash txset_hash{};
  SHA256Hash events_hash{};
  auto db_res = db_client_.dbsched_load_genesis_configuration(
      result.success().genesis_configuration, network_id, txset_hash,
      events_hash);
  ASSERT_EQ(db_res, DbSchedResult::DbSchedResultOK);

  // Initialize the executor with a shard pointing to our mock database server
  IntroExecResult exec_result{
      .exec_id = 1,
      .shards = x23::Vec<ShardCard>{make_shard_card()},
      .event = make_event_card(),
  };
  rpc_server_->impl()->init(std::move(exec_result), available_parallelism());

  // Open the proposal block
  auto open_res = client_.executorsched_open_proposal_block(test_block_id);
  ASSERT_EQ(open_res, ExecResult::ExecResultOK);

  // Open corresponding db block
  db_res = db_client_.dbsched_create_snapshot(
      0, test_block_id, BlockExecutionMode::kBlockExecutionModeProposal);
  ASSERT_EQ(db_res, DbSchedResult::DbSchedResultOK);

  // Get some payment transactions from loadgen
  constexpr size_t kNumTxs = 5;
  LoadgenExtensionResult ext_result =
      loadgen_client_->extend_experiment(context_id, kNumTxs, 1'000'000'000)();
  ASSERT_EQ(ext_result.error(), LoadgenRPCError::kLoadgenRPCErrorOK);

  SignedTransactionBatch txs;
  parallel_batch_to_t(ext_result.success().work_txs, txs)();
  ASSERT_EQ(txs.size(), kNumTxs);

  // Propose each transaction
  for (const auto &stx : txs) {
    auto propose_res =
        client_.executorsched_propose_transaction(test_block_id, stx);
    // Transactions might succeed or fail depending on account balances, etc.
    // But they should not return SystemError if everything is set up correctly
    ASSERT_NE(propose_res.result(), ExecResult::ExecResultSystemError);
  }

  // Finish the block
  client_.executorsched_finish_block(test_block_id);
}

//
// Test proposing a batch of payment transactions from loadgen.
//
TEST_F(RpcLoadgenPaymentTest, TestProposeLoadgenPaymentBatch)
{
  BlockID test_block_id = 200;

  // Generate an experiment with ERC20 payments
  constexpr uint32_t kNumAddresses = 10;
  ExperimentConfig config(ExperimentType::kExperimentTypeERC20Payments);
  config.erc20_payments().num_addresses = kNumAddresses;

  LoadgenResult result = loadgen_client_->generate_experiment(config)();
  ASSERT_EQ(result.error(), LoadgenRPCError::kLoadgenRPCErrorOK);
  LoadgenContextID context_id = result.success().context_id;

  // Load the genesis configuration from loadgen to the database
  uint64_t network_id = 0x0;
  SHA256Hash txset_hash{};
  SHA256Hash events_hash{};
  auto db_res = db_client_.dbsched_load_genesis_configuration(
      result.success().genesis_configuration, network_id, txset_hash,
      events_hash);
  ASSERT_EQ(db_res, DbSchedResult::DbSchedResultOK);

  // Initialize the executor
  IntroExecResult exec_result{
      .exec_id = 1,
      .shards = x23::Vec<ShardCard>{make_shard_card()},
      .event = make_event_card(),
  };
  rpc_server_->impl()->init(std::move(exec_result), available_parallelism());

  // Open the proposal block
  auto open_res = client_.executorsched_open_proposal_block(test_block_id);
  ASSERT_EQ(open_res, ExecResult::ExecResultOK);

  // Open corresponding db block
  db_res = db_client_.dbsched_create_snapshot(
      0, test_block_id, BlockExecutionMode::kBlockExecutionModeProposal);
  ASSERT_EQ(db_res, DbSchedResult::DbSchedResultOK);

  // Get payment transactions from loadgen
  constexpr size_t kNumTxs = 3;
  LoadgenExtensionResult ext_result =
      loadgen_client_->extend_experiment(context_id, kNumTxs, 1'000'000'000)();
  ASSERT_EQ(ext_result.error(), LoadgenRPCError::kLoadgenRPCErrorOK);

  SignedTransactionBatch txs;
  parallel_batch_to_t(ext_result.success().work_txs, txs)();
  ASSERT_EQ(txs.size(), kNumTxs);

  // Propose the batch (signatures included; the executor uses .tx).
  auto batch_result =
      client_.executorsched_propose_batch(test_block_id, txs);
  ASSERT_EQ(batch_result.size(), kNumTxs);

  // Check that all transactions were processed (may succeed or fail, but not
  // SystemError)
  for (const auto &res : batch_result) {
    ASSERT_NE(res.result(), ExecResult::ExecResultSystemError);
  }

  // Finish the block
  client_.executorsched_finish_block(test_block_id);
}

} // namespace hogd
