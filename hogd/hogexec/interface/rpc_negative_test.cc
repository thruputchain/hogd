#include <gtest/gtest.h>

#include <cstdint>
#include <format>
#include <future>
#include <limits>
#include <thread>
#include <utility>
#include <vector>

#include "built_contracts/syscall_misc_tests.h"
#include "consensus.hh"
#include "db_sched.hh"
#include "event_rpc.hh"
#include "exec_result.hh"
#include "exec_sched.hh"
#include "genesis.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/os.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/interface/rpc_from_coord.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/contract_trait_client.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/test_util/block_snapshot_server.h"
#include "hogd/hogexec/runtime/test_util/hogdb_wrapper.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "hogd/version.h"
#include "integration.hh"
#include "proposal.hh"
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
class RpcNegativeTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    server_port_ = rpc_server_.start_server(0);

    client_ = make_srpc_multiclient<ExecutorSched1>(x23::HostPort{"localhost",
                                                                  server_port_},
                                                    false);

    db_client_ = make_srpc_multiclient<DbSched1>(
        x23::HostPort{"localhost", db_wrapper_.coord_port()}, false);
    event_client_ = make_srpc_multiclient<EventSched2>(
        x23::HostPort{"localhost", event_wrapper_.sched_port()}, false);

    ConsensusValue genesis_value{};
    genesis_value.proposal_header.block_height = 0;
    auto db_res =
        db_client_.dbsched_load_genesis_configuration(GenesisConfiguration{},
                                                      genesis_value);
    ASSERT_EQ(db_res, DbSchedResult::DbSchedResultOK)
        << "Failed to load genesis configuration";
  }

  struct StalledSyscallMiscSnapshotSetup {
    ContractAddress contract_address;
    AddressAndKey stalled_addrkey;
  };

  HogdbWrapper db_wrapper_;
  EventServerWrapper event_wrapper_;

  RpcServer<ExecutorMempoolRpc> rpc_server_{default_coropoll()};
  uint16_t server_port_ = 0;

  ExecutorSched1::client<x23::RefPtr<x23::SRpcMulticlient<x23::RpcLogger>>>
      client_;
  DbSched1::client<x23::RefPtr<x23::SRpcMulticlient<x23::RpcLogger>>>
      db_client_;
  EventSched2::client<x23::RefPtr<x23::SRpcMulticlient<x23::RpcLogger>>>
      event_client_;

  x23::Defer<> force_syscallhandler_commit_ =
      SystemCallHandler::force_commit_or_abort();

  ShardCard make_shard_card(uint16_t exec_port = 0)
  {
    if (exec_port == 0) {
      exec_port = db_wrapper_.exec_port();
    }
    return ShardCard{
        .protocol = hogd::version::kHogdProtocol(),
        .upid = {},
        .block_height = 0,
        .address_for_exec = std::format("localhost:{}", exec_port).c_str(),
        .address_for_sched =
            std::format("localhost:{}", db_wrapper_.coord_port()).c_str()};
  }
  EventCard make_event_card() { return event_wrapper_.make_event_card(); }

  void init_executor() { init_executor(make_shard_card()); }

  void init_executor(ShardCard shard_card)
  {
    IntroExecResult exec_result{
        .exec_id = 1,
        .shards = x23::Vec<ShardCard>{std::move(shard_card)},
        .event = make_event_card(),
    };
    rpc_server_->impl()->init(std::move(exec_result), available_parallelism());
  }

  // CHECK, not ASSERT_*: a fatal gtest assertion in a void helper records the
  // failure but lets the caller run on with an unopened block, which fails
  // later in a way that hides the real cause.
  void open_db_block(BlockID block_id)
  {
    auto db_res = db_client_.dbsched_create_snapshot(
        0, block_id, BlockExecutionMode::kBlockExecutionModeProposal);
    CHECK_EQ(db_res, DbSchedResult::DbSchedResultOK);
  }

  void open_event_block(BlockID block_id)
  {
    auto event_res = event_client_.eventsched2_open_block(block_id);
    CHECK_EQ(event_res, RPCError::kRPCNone);
  }

  static StalledSyscallMiscSnapshotSetup
  setup_stalled_syscall_misc_snapshot(BlockSnapshotServer &snapshot_server)
  {
    using syscall_misc_tests_meta =
        ContractTraitMeta<syscall_misc_tests::SyscallMiscTests>;

    auto contract = syscall_misc_tests_meta::load_wasm();
    ContractAddress contract_address =
        NativeContractTestFramework::make_contract_address(1);
    snapshot_server->impl()->setup_create_contract(*contract);
    snapshot_server->impl()->setup_create_deployment(
        contract_address,
        NativeContractTestFramework::make_deployed_contract_hash(contract));

    auto stalled_addrkey = NativeContractTestFramework::
        make_contract_data_addrkey_matching_static_key_sdk_macro(
            contract_address, "nnint_get_key_not_exist");
    snapshot_server->impl()->setup_create_item(
        stalled_addrkey,
        StorageItemWrapper(
            int128_t{7},
            NativeContractTestFramework::get_default_cost_model()));
    snapshot_server->impl()->setup_stall_item(stalled_addrkey);

    return StalledSyscallMiscSnapshotSetup{
        .contract_address = contract_address,
        .stalled_addrkey = stalled_addrkey,
    };
  }

  static Transaction
  make_stalled_syscall_misc_transaction(ContractAddress contract_address)
  {
    ContractTraitClient<syscall_misc_tests::SyscallMiscTests> contract_client(
        contract_address);
    return Transaction{
        .invocation =
            contract_client.make_invocation<"test_nnint_get_key_not_exist">(
                UINT64_MAX),
        .uniquifier = Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = 1},
    };
  }

  // A transaction with the given replay-uniquifier expiry, rooted at an
  // (undeployed) ordinary address.
  static Transaction make_tx_with_expiry(BlockHeight expiry)
  {
    return Transaction{
        .invocation =
            Invocation{.invoked_address =
                           NativeContractTestFramework::make_contract_address(
                               1),
                       .entrypoint = entrypoint_from_method_id(0),
                       .gas_limit = UINT64_MAX,
                       .calldata = {}},
        .uniquifier = Uniquifier{.block_expiry_height = expiry, .nonce = 1},
    };
  }

  // A (non-expired) tx whose serialized size exceeds kMaxBytesSignedTransaction
  // via oversized calldata (#2857).
  static Transaction make_oversized_tx()
  {
    std::vector<uint8_t> big(kMaxBytesSignedTransaction + 1, 0);
    return Transaction{
        .invocation =
            Invocation{.invoked_address =
                           NativeContractTestFramework::make_contract_address(
                               1),
                       .entrypoint = entrypoint_from_method_id(0),
                       .gas_limit = UINT64_MAX,
                       .calldata = Calldata(big.begin(), big.end())},
        .uniquifier = Uniquifier{.block_expiry_height = 1'000'000, .nonce = 1},
    };
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
      client_.executorsched_propose_transaction(block_id, SignedTransaction{});
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
      client_.executorsched_propose_transaction(999999, SignedTransaction{});
  EXPECT_EQ(result.result(), ExecResult::ExecResultSystemError);
}

//
// Expired replay uniquifier: propose() rejects it before the body runs as an
// ExecResultTxValidationFailure (#2568). block_expiry_height 0 is already in
// the past at the execution height (>= 1). The scheduler should not have tried
// to propose an expired transaction; the executor asserts (debug only) that any
// proposal-mode ValidationFailure is in fact a bad expiry height.
//
TEST_F(RpcNegativeTest, ProposeRejectsExpiredReplayUniquifier)
{
  init_executor();

  BlockID block_id = 1010;
  ASSERT_EQ(client_.executorsched_open_proposal_block(block_id),
            ExecResult::ExecResultOK);
  open_db_block(block_id);

  EXPECT_EQ(client_
                .executorsched_propose_transaction(
                    block_id,
                    SignedTransaction{.tx = make_tx_with_expiry(/*expiry=*/0)})
                .result(),
            ExecResult::ExecResultTxValidationFailure);

  client_.executorsched_finish_block(block_id);
}

//
// Expired replay uniquifier on the validation path: validate() rejects it with
// the same ExecResultTxValidationFailure before the body runs (#2568).
//
TEST_F(RpcNegativeTest, ValidateRejectsExpiredReplayUniquifier)
{
  init_executor();

  BlockID block_id = 1011;
  ASSERT_EQ(client_.executorsched_open_validation_block(block_id),
            ExecResult::ExecResultOK);
  auto db_res = db_client_.dbsched_create_snapshot(
      0, block_id, BlockExecutionMode::kBlockExecutionModeValidation);
  ASSERT_EQ(db_res, DbSchedResult::DbSchedResultOK);
  open_event_block(block_id);

  ProposedTransaction proposed_tx{
      .signed_tx = SignedTransaction{.tx = make_tx_with_expiry(/*expiry=*/0)},
      .nondeterministic_proposal_data = {},
  };
  EXPECT_EQ(client_.executorsched_validate_transaction(block_id, proposed_tx),
            ExecResult::ExecResultTxValidationFailure);

  client_.executorsched_finish_block(block_id);
}

// Oversized tx: propose() rejects it before the body runs, so it can't reach
// emit_events and overflow the event RPC (#2857).
TEST_F(RpcNegativeTest, ProposeRejectsOversizedTransaction)
{
  init_executor();

  BlockID block_id = 1020;
  ASSERT_EQ(client_.executorsched_open_proposal_block(block_id),
            ExecResult::ExecResultOK);
  open_db_block(block_id);

  EXPECT_EQ(client_
                .executorsched_propose_transaction(
                    block_id, SignedTransaction{.tx = make_oversized_tx()})
                .result(),
            ExecResult::ExecResultTxNonDeterministicError);

  client_.executorsched_finish_block(block_id);
}

// Same cap on the validation path: a peer-proposed oversized tx is rejected,
// not a validator fail-stop (#2857).
TEST_F(RpcNegativeTest, ValidateRejectsOversizedTransaction)
{
  init_executor();

  BlockID block_id = 1021;
  ASSERT_EQ(client_.executorsched_open_validation_block(block_id),
            ExecResult::ExecResultOK);
  auto db_res = db_client_.dbsched_create_snapshot(
      0, block_id, BlockExecutionMode::kBlockExecutionModeValidation);
  ASSERT_EQ(db_res, DbSchedResult::DbSchedResultOK);
  open_event_block(block_id);

  ProposedTransaction proposed_tx{
      .signed_tx = SignedTransaction{.tx = make_oversized_tx()},
      .nondeterministic_proposal_data = {},
  };
  EXPECT_EQ(client_.executorsched_validate_transaction(block_id, proposed_tx),
            ExecResult::ExecResultTxNonDeterministicError);

  client_.executorsched_finish_block(block_id);
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
  SignedTransactionBatch empty_batch;
  auto result = client_.executorsched_propose_batch(block_id, empty_batch);
  EXPECT_EQ(result.size(), 0);

  client_.executorsched_finish_block(block_id);
}

TEST_F(RpcNegativeTest,
       TestFinishBlockDoesNotRecycleSnapshotCacheDuringValidate)
{
  BlockID block_id = 1005;
  BlockID recycle_probe_block_id = 1006;
  BlockSnapshotServer snapshot_server(block_id);
  auto snapshot_port = snapshot_server.start_server(0);
  auto setup = setup_stalled_syscall_misc_snapshot(snapshot_server);
  x23::Defer cleanup([&] {
    snapshot_server->impl()->release_stalled_item(setup.stalled_addrkey);
  });

  init_executor(make_shard_card(snapshot_port));
  ASSERT_EQ(client_.executorsched_open_validation_block(block_id),
            ExecResult::ExecResultOK);
  open_event_block(block_id);

  ProposedTransactionBatch batch;
  batch.push_back(ProposedTransaction{
      .signed_tx =
          SignedTransaction{
              .tx =
                  make_stalled_syscall_misc_transaction(setup.contract_address),
          },
      .nondeterministic_proposal_data = {},
  });

  auto validate_client = make_srpc_multiclient<ExecutorSched1>(
      x23::HostPort{"localhost", server_port_}, false);
  std::promise<ExecValidateBatchResult> validate_promise;
  auto validate_future = validate_promise.get_future();
  std::thread validate_thread([&] {
    validate_promise.set_value(
        validate_client.executorsched_validate_batch(block_id, batch));
  });

  snapshot_server->impl()->wait_for_stalled_item(setup.stalled_addrkey);
  EXPECT_EQ(client_.executorsched_finish_block(block_id),
            ExecResult::ExecResultOK);

  // If finish_block() recycles block-owned resources before the stalled
  // validation resumes, opening another block can reuse the same snapshot
  // cache and retarget it to the new block ID. Under the buggy validate_batch
  // ordering that released the executor context before awaiting the batch,
  // resuming this stalled validation then trips UAF or reads through the
  // wrong block snapshot. Sanitized builds catch that old behavior reliably.
  ASSERT_EQ(client_.executorsched_open_validation_block(recycle_probe_block_id),
            ExecResult::ExecResultOK);
  open_event_block(recycle_probe_block_id);

  snapshot_server->impl()->release_stalled_item(setup.stalled_addrkey);
  auto result = validate_future.get();
  ASSERT_EQ(result.size(), 1);
  EXPECT_EQ(result[0], ExecResult::ExecResultTxDeterministicError);
  EXPECT_EQ(client_.executorsched_finish_block(recycle_probe_block_id),
            ExecResult::ExecResultOK);
  EXPECT_EQ(event_client_
                .eventsched2_close_block(block_id, block_id, ProposalTxs{})
                .error(),
            RPCError::kRPCNone);
  EXPECT_EQ(event_client_
                .eventsched2_close_block(recycle_probe_block_id,
                                         recycle_probe_block_id, ProposalTxs{})
                .error(),
            RPCError::kRPCNone);
  validate_thread.join();
}

TEST_F(RpcNegativeTest, TestFinishBlockDoesNotRecycleSnapshotCacheDuringPropose)
{
  BlockID block_id = 1007;
  BlockID recycle_probe_block_id = 1008;
  BlockSnapshotServer snapshot_server(block_id);
  auto snapshot_port = snapshot_server.start_server(0);
  auto setup = setup_stalled_syscall_misc_snapshot(snapshot_server);
  x23::Defer cleanup([&] {
    snapshot_server->impl()->release_stalled_item(setup.stalled_addrkey);
  });

  init_executor(make_shard_card(snapshot_port));
  ASSERT_EQ(client_.executorsched_open_proposal_block(block_id),
            ExecResult::ExecResultOK);
  open_event_block(block_id);

  SignedTransactionBatch batch;
  batch.push_back(SignedTransaction{
      .tx = make_stalled_syscall_misc_transaction(setup.contract_address)});

  auto propose_client = make_srpc_multiclient<ExecutorSched1>(
      x23::HostPort{"localhost", server_port_}, false);
  std::promise<ExecProposeBatchResult> propose_promise;
  auto propose_future = propose_promise.get_future();
  std::thread propose_thread([&] {
    propose_promise.set_value(
        propose_client.executorsched_propose_batch(block_id, batch));
  });

  snapshot_server->impl()->wait_for_stalled_item(setup.stalled_addrkey);
  EXPECT_EQ(client_.executorsched_finish_block(block_id),
            ExecResult::ExecResultOK);

  // This mirrors the validate regression but exercises the propose path.
  // propose_batch did not have the old validate-specific await ordering bug,
  // but if finish_block() ever regresses and recycles block-owned resources
  // while a stalled propose still holds a context ref, opening another block
  // can retarget the recycled snapshot cache. Sanitized builds should catch
  // that shutdown/GC regression here.
  ASSERT_EQ(client_.executorsched_open_proposal_block(recycle_probe_block_id),
            ExecResult::ExecResultOK);
  open_event_block(recycle_probe_block_id);

  snapshot_server->impl()->release_stalled_item(setup.stalled_addrkey);
  auto result = propose_future.get();
  ASSERT_EQ(result.size(), 1);
  EXPECT_EQ(result[0].result(), ExecResult::ExecResultTxDeterministicError);
  EXPECT_EQ(client_.executorsched_finish_block(recycle_probe_block_id),
            ExecResult::ExecResultOK);
  EXPECT_EQ(event_client_
                .eventsched2_close_block(block_id, block_id, ProposalTxs{})
                .error(),
            RPCError::kRPCNone);
  EXPECT_EQ(event_client_
                .eventsched2_close_block(recycle_probe_block_id,
                                         recycle_probe_block_id, ProposalTxs{})
                .error(),
            RPCError::kRPCNone);
  propose_thread.join();
}

} // namespace hogd
