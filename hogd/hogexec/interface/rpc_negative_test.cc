#include <gtest/gtest.h>

#include <format>
#include <future>
#include <stdexcept>
#include <thread>
#include <utility>

#include "db_sched.hh"
#include "exec_result.hh"
#include "exec_sched.hh"
#include "genesis.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/os.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
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

class RpcNegativeTest : public ::testing::Test {
public:
  void SetUp() override
  {
    server_port_ = rpc_server_.start_server(0);

    client_ = make_srpc_multiclient<ExecutorSched1>(x23::HostPort{"localhost",
                                                                  server_port_},
                                                    false);

    db_client_ = make_srpc_multiclient<DbSched1>(
        x23::HostPort{"localhost", db_wrapper_.coord_port()}, false);

    uint64_t network_id = 0x0;
    SHA256Hash txset_hash{};
    SHA256Hash events_hash{};
    auto db_res =
        db_client_.dbsched_load_genesis_configuration(GenesisConfiguration{},
                                                      network_id, txset_hash,
                                                      events_hash);
    if (db_res != DbSchedResult::DbSchedResultOK) {
      throw std::runtime_error("Failed to load genesis configuration");
    }
  }

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
        .address_for_exec =
            std::format("localhost:{}", db_wrapper_.exec_port()).c_str(),
        .address_for_sched =
            std::format("localhost:{}", db_wrapper_.coord_port()).c_str()};
  }
  EventCard make_event_card() { return event_wrapper_.make_event_card(); }

  void init_executor()
  {
    IntroExecResult exec_result{
        .exec_id = 1,
        .shards = x23::Vec<ShardCard>{make_shard_card()},
        .event = make_event_card(),
    };
    rpc_server_->impl()->init(std::move(exec_result), available_parallelism());
  }

  void open_db_block(BlockID block_id)
  {
    auto db_res = db_client_.dbsched_create_snapshot(
        0, block_id, BlockExecutionMode::kBlockExecutionModeProposal);
    if (db_res != DbSchedResult::DbSchedResultOK) {
      throw std::runtime_error("Failed to open db block");
    }
  }
};

//
// 1. Mode mismatch: open a proposal block, then try to validate on it.
//
TEST_F(RpcNegativeTest, TestModeMismatchProposeOnValidationBlock)
{
  init_executor();

  BlockID block_id = 1000;
  ASSERT_EQ(client_.executorsched_open_validation_block(block_id),
            ExecResult::ExecResultOK);

  // Proposing on a validation block should fail
  auto result =
      client_.executorsched_propose_transaction(block_id, Transaction{});
  EXPECT_EQ(result.result(), ExecResult::ExecResultSystemError);

  client_.executorsched_finish_block(block_id);
}

//
// 2. Mode mismatch: open a proposal block, then try to validate on it
//    (reverse direction).
//
TEST_F(RpcNegativeTest, TestModeMismatchValidateOnProposalBlock)
{
  init_executor();

  BlockID block_id = 1001;
  ASSERT_EQ(client_.executorsched_open_proposal_block(block_id),
            ExecResult::ExecResultOK);

  open_db_block(block_id);

  // Validating on a proposal block should fail
  ProposedTransaction proposed_tx{};
  auto result =
      client_.executorsched_validate_transaction(block_id, proposed_tx);
  EXPECT_EQ(result, ExecResult::ExecResultSystemError);

  client_.executorsched_finish_block(block_id);
}

//
// 3. Conflicting mode open: open proposal, then open validation with same ID.
//
TEST_F(RpcNegativeTest, TestConflictingModeOpen)
{
  init_executor();

  BlockID block_id = 1002;
  ASSERT_EQ(client_.executorsched_open_proposal_block(block_id),
            ExecResult::ExecResultOK);

  // Opening a validation block with the same ID should fail
  EXPECT_EQ(client_.executorsched_open_validation_block(block_id),
            ExecResult::ExecResultSystemError);

  client_.executorsched_finish_block(block_id);
}

//
// 4. Propose to a non-existent block.
//
TEST_F(RpcNegativeTest, TestProposeToNonExistentBlock)
{
  init_executor();

  // Block 999999 was never opened
  auto result =
      client_.executorsched_propose_transaction(999999, Transaction{});
  EXPECT_EQ(result.result(), ExecResult::ExecResultSystemError);
}

//
// 5. Double finish: open once, finish twice. Second finish should error.
//
TEST_F(RpcNegativeTest, TestDoubleFinish)
{
  init_executor();

  BlockID block_id = 1003;
  ASSERT_EQ(client_.executorsched_open_proposal_block(block_id),
            ExecResult::ExecResultOK);

  EXPECT_EQ(client_.executorsched_finish_block(block_id),
            ExecResult::ExecResultOK);

  // Second finish on the same block should fail (block already closed / GC'd)
  EXPECT_EQ(client_.executorsched_finish_block(block_id),
            ExecResult::ExecResultSystemError);
}

//
// 6. Finish a block that was never opened.
//
TEST_F(RpcNegativeTest, TestFinishNonExistentBlock)
{
  init_executor();

  EXPECT_EQ(client_.executorsched_finish_block(888888),
            ExecResult::ExecResultSystemError);
}

//
// 7. Propose an empty batch.
//
TEST_F(RpcNegativeTest, TestEmptyBatch)
{
  init_executor();

  BlockID block_id = 1004;
  ASSERT_EQ(client_.executorsched_open_proposal_block(block_id),
            ExecResult::ExecResultOK);

  open_db_block(block_id);

  // Empty batch should return an empty result, not crash
  TransactionBatch empty_batch;
  auto result = client_.executorsched_propose_batch(block_id, empty_batch);
  EXPECT_EQ(result.size(), 0);

  client_.executorsched_finish_block(block_id);
}

TEST_F(RpcNegativeTest, TestFinishBlockDoesNotGCWhileValidateBatchInFlight)
{
  init_executor();

  BlockID block_id = 1005;
  ASSERT_EQ(client_.executorsched_open_validation_block(block_id),
            ExecResult::ExecResultOK);

  ExecutorStateManager::reset_gc_count_for_test();
  ExecutorContext::block_validate_transaction_batch_for_test();
  x23::Defer cleanup([&] {
    ExecutorContext::unblock_validate_transaction_batch_for_test();
    ExecutorContext::reset_validate_transaction_batch_test_state();
  });

  std::promise<ExecValidateBatchResult> validate_promise;
  auto validate_future = validate_promise.get_future();
  std::thread validate_thread([&] {
    ProposedTransactionBatch empty_batch;
    validate_promise.set_value(
        client_.executorsched_validate_batch(block_id, empty_batch));
  });

  ExecutorContext::wait_for_validate_transaction_batch_blocked_for_test();

  EXPECT_EQ(client_.executorsched_finish_block(block_id),
            ExecResult::ExecResultOK);
  EXPECT_EQ(ExecutorStateManager::gc_count_for_test(), 0U);

  ExecutorContext::unblock_validate_transaction_batch_for_test();
  EXPECT_TRUE(validate_future.get().empty());
  validate_thread.join();

  EXPECT_EQ(ExecutorStateManager::gc_count_for_test(), 1U);
}

} // namespace hogd
