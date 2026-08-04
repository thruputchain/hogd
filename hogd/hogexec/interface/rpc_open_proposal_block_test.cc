#include "hogd/hogexec/test_main.h"

#include <format>
#include <gtest/gtest.h>
#include <limits>
#include <utility>

#include "consensus.hh"
#include "db_sched.hh"
#include "exec_result.hh"
#include "exec_sched.hh"
#include "genesis.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/os.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/db_shard_routing/sharding.h" // kShardFunctionId
#include "hogd/hogexec/interface/rpc_from_coord.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/test_util/hogdb_wrapper.h"
#include "hogd/version.h"
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

// NOLINTNEXTLINE(misc-use-internal-linkage)
class RpcOpenProposalBlockTest : public ::testing::Test {
public:
  void SetUp() override
  {
    // Start RPC server on a random port
    server_port_ = rpc_server_.start_server(0);

    // Create RPC client connected to the server
    client_ = make_srpc_multiclient<ExecutorSched1>(x23::HostPort{"localhost",
                                                                  server_port_},
                                                    false);

    db_client_ = make_srpc_multiclient<DbSched1>(
        x23::HostPort{"localhost", db_wrapper_.coord_port()}, false);

    ConsensusValue genesis_value{};
    genesis_value.proposal_header.block_height = 0;
    auto db_res =
        db_client_.dbsched_load_genesis_configuration(GenesisConfiguration{},
                                                      genesis_value);
    ASSERT_EQ(db_res, DbSchedResult::DbSchedResultOK)
        << "Failed to load genesis configuration";
  }

  // void TearDown() override { rpc_server_.shut_down(); }

protected:
  HogdbWrapper db_wrapper_;
  EventServerWrapper event_wrapper_;

  RpcServer<ExecutorMempoolRpc> rpc_server_{default_coropoll()};
  uint16_t server_port_ = 0;

  ExecutorSched1::client<x23::RefPtr<x23::SRpcMulticlient<x23::RpcLogger>>>
      client_;
  DbSched1::client<x23::RefPtr<x23::SRpcMulticlient<x23::RpcLogger>>>
      db_client_;

  x23::Defer<> force_syscallhandler_commit_ =
      SystemCallHandler::force_commit_or_abort();

  ShardCard make_shard_card()
  {
    return ShardCard{
        .protocol = hogd::version::kHogdProtocol(),
        .upid = {},
        .block_height = 0,
        .shard_id = 0,
        .total_shards = 1,
        .shard_function_id = kShardFunctionId,
        .address_for_exec =
            std::format("localhost:{}", db_wrapper_.exec_port()).c_str(),
        .address_for_sched =
            std::format("localhost:{}", db_wrapper_.coord_port()).c_str()};
  }
  EventCard make_event_card() { return event_wrapper_.make_event_card(); }
};

//
// Test that open_proposal_block returns SystemError before init().
//
TEST_F(RpcOpenProposalBlockTest, TestOpenProposalBlockBeforeInit)
{
  BlockID test_block_id = 123;

  // Call open_proposal_block before init - should fail
  auto result = client_.executorsched_open_proposal_block(test_block_id);

  // Should return system error since executor is not initialized
  EXPECT_EQ(result, ExecResult::ExecResultSystemError);
}

//
// Test that open_proposal_block returns OK after init().
//
TEST_F(RpcOpenProposalBlockTest, TestOpenProposalBlockAfterInit)
{
  BlockID test_block_id = 456;

  // Create a mock IntroExecResult for initialization
  IntroExecResult exec_result{
      .exec_id = 1,
      .shards = x23::Vec<ShardCard>{make_shard_card()},
      .event = make_event_card(),
  };

  // Initialize the executor with a shard pointing to our mock database server
  rpc_server_->impl()->init(std::move(exec_result), available_parallelism());

  // Call open_proposal_block after init - should succeed
  auto result = client_.executorsched_open_proposal_block(test_block_id);

  // Should return OK since executor is now initialized
  EXPECT_EQ(result, ExecResult::ExecResultOK);
}

//
// Test open_proposal_block refcounting and finish_block behavior.
//
TEST_F(RpcOpenProposalBlockTest, TestOpenProposalBlockMultipleCalls)
{
  BlockID test_block_id = 789;

  IntroExecResult exec_result;
  exec_result.exec_id = 1;

  exec_result.shards = x23::Vec<ShardCard>{make_shard_card()};
  exec_result.event = make_event_card();

  // Initialize the executor with a shard pointing to our mock database server
  rpc_server_->impl()->init(std::move(exec_result), available_parallelism());

  // Call open_proposal_block multiple times - should all succeed
  auto result1 = client_.executorsched_open_proposal_block(test_block_id);
  auto result2 = client_.executorsched_open_proposal_block(test_block_id);
  auto result3 = client_.executorsched_open_proposal_block(test_block_id);

  // open corresponding db block
  // Without this, the executor's calls to lookup deployed contracts will fail
  // (with bad block id, instead of the expected "contract not found")
  auto db_res = db_client_.dbsched_create_snapshot(
      0, test_block_id, BlockExecutionMode::kBlockExecutionModeProposal);
  ASSERT_EQ(db_res, DbSchedResult::DbSchedResultOK)
      << "Failed to open db block";

  // All calls should return OK
  EXPECT_EQ(result1, ExecResult::ExecResultOK);
  EXPECT_EQ(result2, ExecResult::ExecResultOK);
  EXPECT_EQ(result3, ExecResult::ExecResultOK);

  ASSERT_EQ(client_.executorsched_finish_block(test_block_id),
            ExecResult::ExecResultOK);
  ASSERT_EQ(client_.executorsched_finish_block(test_block_id),
            ExecResult::ExecResultOK);

  // A transaction rooted at the undeployed zero address is never included:
  //   - with a non-expired uniquifier the body faults (no contract at the zero
  //     address) and is recorded as a deterministic fault;
  EXPECT_EQ(
      client_
          .executorsched_propose_transaction(
              test_block_id,
              SignedTransaction{
                  .tx = Transaction{.uniquifier = {.block_expiry_height =
                                                       std::numeric_limits<
                                                           BlockHeight>::max(),
                                                   .nonce = 1}}})
          .result(),
      ExecResult::ExecResultTxDeterministicError);

  //   - with the default {0,0} (expired) uniquifier the pre-body expiry check
  //     rejects it before the body even runs, as an
  //     ExecResultTxValidationFailure (the scheduler should not have proposed
  //     it), so it is not included
  //     (#2568).
  EXPECT_EQ(client_
                .executorsched_propose_transaction(test_block_id,
                                                   SignedTransaction{})
                .result(),
            ExecResult::ExecResultTxValidationFailure);

  client_.executorsched_finish_block(test_block_id);

  EXPECT_EQ(client_
                .executorsched_propose_transaction(test_block_id,
                                                   SignedTransaction{})
                .result(),
            ExecResult::ExecResultSystemError);
}

//
// Test open_proposal_block refcounting and finish_block behavior.
//
TEST_F(RpcOpenProposalBlockTest, TestProposeBatch)
{
  BlockID test_block_id = 789;

  IntroExecResult exec_result;
  exec_result.exec_id = 1;

  exec_result.shards = x23::Vec<ShardCard>{make_shard_card()};
  exec_result.event = make_event_card();

  // Initialize the executor with a shard pointing to our mock database server
  rpc_server_->impl()->init(std::move(exec_result), available_parallelism());

  EXPECT_EQ(client_.executorsched_open_proposal_block(test_block_id),
            ExecResult::ExecResultOK);

  SignedTransactionBatch txs = {SignedTransaction{}, SignedTransaction{},
                                SignedTransaction{}};
  auto result = client_.executorsched_propose_batch(test_block_id, txs);
  ASSERT_EQ(result.size(), 3);
  for (auto &res : result) {
    ASSERT_NE(res.result(), ExecResult::ExecResultOK);
  }

  client_.executorsched_finish_block(test_block_id);
}

} // namespace hogd

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  return hogd::run_hogexec_tests(argc, argv);
}
