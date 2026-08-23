#include <gtest/gtest.h>

#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "consensus.hh"
#include "contract.hh"
#include "db_sched.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_exec/common/limits.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/components/borrowed_contract_batch.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/event_server_proxy.h"
#include "hogd/hog_exec/runtime/item/storage_snapshot_cache.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/ramdb/constants.h"
#include "hogd/ramdb/db.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/serialize.h"

namespace hogd {

// NOLINTNEXTLINE(misc-use-internal-linkage)
class DirectStalenessTest : public ::testing::Test,
                            public NativeContractTestFramework {
public:
  DirectStalenessTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb)
  {}

  struct SiblingBlocks {
    BlockID block_a;
    BlockID block_b;
  };

  SiblingBlocks create_sibling()
  {
    BlockID block_a = get_block_number();
    BlockID parent_id = get_db_tip().id;

    constexpr BlockID kSiblingOffset = 1000;
    BlockID block_b = block_a + kSiblingOffset;
    auto r = get_coord_client().dbsched_create_snapshot(
        parent_id, block_b, BlockExecutionMode::kBlockExecutionModeProposal);
    EXPECT_EQ(r, DbSchedResult::DbSchedResultOK);

    return {.block_a = block_a, .block_b = block_b};
  }

  void close_and_commit_block(BlockID block)
  {
    auto close = get_coord_client().dbsched_close_snapshot(block);
    EXPECT_EQ(close.result(), DbSchedResult::DbSchedResultOK);

    ConsensusValue value{};
    value.proposal_header.block_height = block;
    auto commit = get_coord_client().dbsched_commit_snapshot(block, value);
    EXPECT_EQ(commit.result(), DbSchedResult::DbSchedResultOK);
  }

  SiblingBlocks create_and_commit_sibling()
  {
    auto blocks = create_sibling();
    close_and_commit_block(blocks.block_a);
    return blocks;
  }
};

TEST_F(DirectStalenessTest, SnapshotItemReadOnStaleBlockReturnsAbandon)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto key = make_contract_data_addrkey(contract_addr, "test_key");

  {
    auto tx = make_new_per_tx_db_proxy(0);
    tx.add_delta(key, make_nnint_set_add_delta(0, 42),
                 std::to_underlying(StorageDeltaFlags::None));
    ASSERT_TRUE(tx.try_push_current_subtransaction()().has_value());
    ASSERT_TRUE(tx.commit_transaction()().has_value());
  }

  advance_block();

  auto [block_a, block_b] = create_and_commit_sibling();

  auto res = get_db_proxy().get_snapshot_item(block_b, key)();
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), SystemErrorCode::Abandon);
}

TEST_F(DirectStalenessTest, ContractReadOnStaleBlockReturnsAbandon)
{
  auto contract = make_native_contract(0);
  write_contract(contract);

  advance_block();

  auto [block_a, block_b] = create_and_commit_sibling();

  auto res = get_db_proxy().get_contract(block_b, sha256(*contract))();
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), SystemErrorCode::Abandon);
}

TEST_F(DirectStalenessTest, DeploymentReadOnStaleBlockReturnsAbandon)
{
  auto contract_addr = make_contract_address(1);
  auto contract = make_native_contract(0);
  write_deployment(contract_addr, contract);

  advance_block();

  auto [block_a, block_b] = create_and_commit_sibling();

  auto res = get_db_proxy().get_deployment(block_b, contract_addr)();
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), SystemErrorCode::Abandon);
}

TEST_F(DirectStalenessTest, DbTipReadOnStaleBlockReturnsAbandon)
{
  advance_block();

  auto [block_a, block_b] = create_and_commit_sibling();

  auto stale_snapshot_res =
      get_db_proxy().get_contract(block_b, SHA256Hash{})();
  ASSERT_FALSE(stale_snapshot_res.has_value());
  EXPECT_EQ(stale_snapshot_res.error().code(), SystemErrorCode::Abandon);

  auto res = get_db_proxy().get_db_tip(block_b)();
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), SystemErrorCode::Abandon);
}

TEST_F(DirectStalenessTest,
       StaleNewContractCommitReturnsAbandonAndReleasesBlock)
{
  auto contract_addr = make_contract_address(2);
  setup_grant_storage_token(contract_addr, 1'000'000'000);
  advance_block();

  auto [block_a, block_b] = create_sibling();

  Contract contract(ContractType::kRv32);
  contract.rv32_contract().data = {0x01, 0x02, 0x03, 0x04,
                                   0x01, 0x00, 0x00, 0x00};
  auto contract_ptr = std::make_shared<const Contract>(contract);
  auto deployed_contract_hash = make_deployed_contract_hash(contract_ptr);

  constexpr TransactionID kTxId = 42;
  PerTransactionDatabaseProxy tx(get_db_proxy(), block_b, kTxId);
  AddressAndKey addrkey{.address = contract_addr,
                        .key = StorageKey(StorageKeyType::Metadata)};
  StorageDelta delta(StorageDeltaType::kDeltaTypeDeployContract);
  delta.deployed_contract_hash() = deployed_contract_hash;
  tx.add_delta(addrkey, std::move(delta),
               std::to_underlying(StorageDeltaFlags::None));
  auto push = tx.try_push_current_subtransaction()();
  ASSERT_TRUE(push.has_value());
  ASSERT_TRUE(push.value());

  close_and_commit_block(block_a);

  BorrowedContractBatch::hash_set_t hashes{deployed_contract_hash.hash};
  BorrowedContractBatch::contract_map_t contracts;
  contracts.emplace(deployed_contract_hash.hash,
                    std::make_unique<const Contract>(contract));
  BorrowedContractBatch new_contracts(hashes, contracts);

  auto commit = tx.commit_transaction(new_contracts)();
  ASSERT_FALSE(commit.has_value());
  EXPECT_EQ(commit.error().code(), SystemErrorCode::Abandon);

  // The invoke transaction reference should have been released, so the first
  // abandon actually removes the stale block and the second abandon fails to
  // find it.
  auto abandon = get_coord_client().dbsched_abandon_snapshot(block_b);
  EXPECT_EQ(abandon, DbSchedResult::DbSchedResultOK);
  auto second_abandon = get_coord_client().dbsched_abandon_snapshot(block_b);
  EXPECT_EQ(second_abandon, DbSchedResult::DbSchedResultError);
}

// Regression test for issue #2599.
//
// Same shape as StaleNewContractCommitReturnsAbandonAndReleasesBlock, with a
// contract body that exceeds one RAMDB item (stored chunked since #2569). On
// an invalidated block the commit fails at the stale-aware lock and must
// return Abandon, exactly like the small-contract control.
TEST_F(DirectStalenessTest, StaleOversizedNewContractCommitReturnsAbandon)
{
  auto contract_addr = make_contract_address(3);
  setup_grant_storage_token(contract_addr, 1'000'000'000);
  advance_block();

  auto [block_a, block_b] = create_sibling();

  Contract contract(ContractType::kRv32);
  auto &data = contract.rv32_contract().data;
  data = {0x01, 0x02, 0x03, 0x04};
  data.resize(data.size() + kBigItemThreshold);
  auto contract_ptr = std::make_shared<const Contract>(contract);
  auto deployed_contract_hash = make_deployed_contract_hash(contract_ptr);

  // The serialized contract exceeds RAMDB's single-item limit (it would be
  // stored chunked, #2569). The stale-aware lock fires on the first chunk
  // write, returning kStaleBlock -- the Abandon path is identical to the
  // small-contract case.
  ASSERT_FALSE(Database::item_fits(
      make_wasm_code_key(deployed_contract_hash.hash).size(),
      x23::xdr_size(contract)));

  constexpr TransactionID kTxId = 43;
  PerTransactionDatabaseProxy tx(get_db_proxy(), block_b, kTxId);
  AddressAndKey addrkey{.address = contract_addr,
                        .key = StorageKey(StorageKeyType::Metadata)};
  StorageDelta delta(StorageDeltaType::kDeltaTypeDeployContract);
  delta.deployed_contract_hash() = deployed_contract_hash;
  tx.add_delta(addrkey, std::move(delta),
               std::to_underlying(StorageDeltaFlags::None));
  auto push = tx.try_push_current_subtransaction()();
  ASSERT_TRUE(push.has_value());
  ASSERT_TRUE(push.value());

  close_and_commit_block(block_a);

  BorrowedContractBatch::hash_set_t hashes{deployed_contract_hash.hash};
  BorrowedContractBatch::contract_map_t contracts;
  contracts.emplace(deployed_contract_hash.hash,
                    std::make_unique<const Contract>(contract));
  BorrowedContractBatch new_contracts(hashes, contracts);

  auto commit = tx.commit_transaction(new_contracts)();
  ASSERT_FALSE(commit.has_value());
  EXPECT_EQ(commit.error().code(), SystemErrorCode::Abandon);

  // The commit released the invoke transaction's block reference, so the first
  // abandon removes the stale block and the second no longer finds it.
  auto abandon = get_coord_client().dbsched_abandon_snapshot(block_b);
  EXPECT_EQ(abandon, DbSchedResult::DbSchedResultOK);
  auto second_abandon = get_coord_client().dbsched_abandon_snapshot(block_b);
  EXPECT_EQ(second_abandon, DbSchedResult::DbSchedResultError);
}

// A row above kMaxSerializedContractBytes on an already-invalidated block:
// the preflight rejects it (kInvalidExecutorBehavior) before the stale-aware
// lock, and the staleness re-check in commit_transaction must still win,
// returning Abandon rather than Unrecoverable (#2599, #2569).
TEST_F(DirectStalenessTest, StaleOverlimitNewContractCommitReturnsAbandon)
{
  auto contract_addr = make_contract_address(4);
  setup_grant_storage_token(contract_addr, 1'000'000'000);
  advance_block();

  auto [block_a, block_b] = create_sibling();

  Contract contract(ContractType::kRv32);
  auto &data = contract.rv32_contract().data;
  data = {0x01, 0x02, 0x03, 0x04};
  data.resize(data.size() + kMaxSerializedContractBytes);
  auto contract_ptr = std::make_shared<const Contract>(contract);
  auto deployed_contract_hash = make_deployed_contract_hash(contract_ptr);

  ASSERT_GT(x23::xdr_size(contract), kMaxSerializedContractBytes);

  constexpr TransactionID kTxId = 44;
  PerTransactionDatabaseProxy tx(get_db_proxy(), block_b, kTxId);
  AddressAndKey addrkey{.address = contract_addr,
                        .key = StorageKey(StorageKeyType::Metadata)};
  StorageDelta delta(StorageDeltaType::kDeltaTypeDeployContract);
  delta.deployed_contract_hash() = deployed_contract_hash;
  tx.add_delta(addrkey, std::move(delta),
               std::to_underlying(StorageDeltaFlags::None));
  auto push = tx.try_push_current_subtransaction()();
  ASSERT_TRUE(push.has_value());
  ASSERT_TRUE(push.value());

  close_and_commit_block(block_a);

  BorrowedContractBatch::hash_set_t hashes{deployed_contract_hash.hash};
  BorrowedContractBatch::contract_map_t contracts;
  contracts.emplace(deployed_contract_hash.hash,
                    std::make_unique<const Contract>(contract));
  BorrowedContractBatch new_contracts(hashes, contracts);

  auto commit = tx.commit_transaction(new_contracts)();
  ASSERT_FALSE(commit.has_value());
  EXPECT_EQ(commit.error().code(), SystemErrorCode::Abandon);

  // request_new_contract returned early (kInvalidExecutorBehavior) without
  // acquiring any lock, but commit_transaction still aborted and closed the
  // invoke tx, releasing the block reference: the first abandon removes the
  // stale block and the second no longer finds it.
  auto abandon = get_coord_client().dbsched_abandon_snapshot(block_b);
  EXPECT_EQ(abandon, DbSchedResult::DbSchedResultOK);
  auto second_abandon = get_coord_client().dbsched_abandon_snapshot(block_b);
  EXPECT_EQ(second_abandon, DbSchedResult::DbSchedResultError);
}

TEST_F(DirectStalenessTest, CommitInvalidatedBlockReturnsError)
{
  advance_block();

  auto [block_a, block_b] = create_sibling();

  close_and_commit_block(block_a);

  auto close_b = get_coord_client().dbsched_close_snapshot(block_b);
  EXPECT_EQ(close_b.result(), DbSchedResult::DbSchedResultOK);

  ConsensusValue value_b{};
  value_b.proposal_header.block_height = block_b;
  auto commit_b = get_coord_client().dbsched_commit_snapshot(block_b, value_b);
  EXPECT_NE(commit_b.result(), DbSchedResult::DbSchedResultOK);
}

// Viewing-snapshot refresh tests.
//
// These verify that opening a fresh viewing snapshot after a commit
// returns current data, even when a previous snapshot's
// StorageSnapshotCache was warm with stale entries.  This is the
// mechanism that ViewGatewayRpc::request_refresh() relies on.

TEST_F(DirectStalenessTest, FreshViewingSnapshotSeesUpdatedContractData)
{
  auto contract_addr = make_contract_address(10);
  setup_grant_storage_token(contract_addr, 1'000'000'000);
  auto addrkey = make_contract_data_addrkey(contract_addr, "view_key");

  {
    auto tx = make_new_per_tx_db_proxy(0);
    tx.add_delta(addrkey, make_bytestring_delta("old-value"),
                 std::to_underlying(StorageDeltaFlags::None));
    ASSERT_TRUE(tx.try_push_current_subtransaction()().has_value());
    ASSERT_TRUE(tx.commit_transaction()().has_value());
  }

  advance_block();

  auto snap1 = get_db_proxy().open_viewing_snapshot()();
  ASSERT_TRUE(snap1.has_value());
  StorageSnapshotCache cache1(snap1->viewing_block_id, get_db_proxy());

  auto warm = cache1.try_get_snapshot_item(addrkey)();
  ASSERT_TRUE(warm.has_value());
  ASSERT_NE(warm.value(), nullptr);
  EXPECT_EQ(warm.value()->bytestring(), make_bytestring("old-value"));

  {
    auto tx = make_new_per_tx_db_proxy(1);
    tx.add_delta(addrkey, make_bytestring_delta("new-value"),
                 std::to_underlying(StorageDeltaFlags::None));
    ASSERT_TRUE(tx.try_push_current_subtransaction()().has_value());
    ASSERT_TRUE(tx.commit_transaction()().has_value());
  }

  advance_block();

  // The stale cache still returns old-value.
  auto stale = cache1.try_get_snapshot_item(addrkey)();
  ASSERT_TRUE(stale.has_value());
  ASSERT_NE(stale.value(), nullptr);
  EXPECT_EQ(stale.value()->bytestring(), make_bytestring("old-value"));

  // A fresh viewing snapshot sees new-value.
  auto snap2 = get_db_proxy().open_viewing_snapshot()();
  ASSERT_TRUE(snap2.has_value());
  StorageSnapshotCache cache2(snap2->viewing_block_id, get_db_proxy());

  auto fresh = cache2.try_get_snapshot_item(addrkey)();
  ASSERT_TRUE(fresh.has_value());
  ASSERT_NE(fresh.value(), nullptr);
  EXPECT_EQ(fresh.value()->bytestring(), make_bytestring("new-value"));

  EXPECT_TRUE(get_db_proxy()
                  .close_viewing_snapshot(snap2->viewing_block_id)()
                  .has_value());
  EXPECT_TRUE(get_db_proxy()
                  .close_viewing_snapshot(snap1->viewing_block_id)()
                  .has_value());
}

TEST_F(DirectStalenessTest, FreshViewingSnapshotSeesNewlyDeployedContract)
{
  auto contract_addr = make_contract_address(11);
  auto missing_hash = sha256(std::string("contract-not-yet-deployed"));

  advance_block();

  auto snap1 = get_db_proxy().open_viewing_snapshot()();
  ASSERT_TRUE(snap1.has_value());
  StorageSnapshotCache cache1(snap1->viewing_block_id, get_db_proxy());

  // Warm negative contract lookup.
  auto warm = cache1.try_get_contract_by_hash(missing_hash)();
  ASSERT_TRUE(warm.has_value());
  EXPECT_EQ(warm.value(), nullptr);

  // Warm negative deployment lookup.
  auto warm_deploy = cache1.try_get_deployed_contract(contract_addr)();
  ASSERT_TRUE(warm_deploy.has_value());
  EXPECT_EQ(warm_deploy.value(), nullptr);

  // Now deploy the contract and advance.
  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);

  advance_block();

  // Stale cache still returns nulls.
  auto stale_contract = cache1.try_get_contract_by_hash(missing_hash)();
  ASSERT_TRUE(stale_contract.has_value());
  EXPECT_EQ(stale_contract.value(), nullptr);

  auto stale_deploy = cache1.try_get_deployed_contract(contract_addr)();
  ASSERT_TRUE(stale_deploy.has_value());
  EXPECT_EQ(stale_deploy.value(), nullptr);

  // Fresh viewing snapshot sees the deployment.
  auto snap2 = get_db_proxy().open_viewing_snapshot()();
  ASSERT_TRUE(snap2.has_value());
  StorageSnapshotCache cache2(snap2->viewing_block_id, get_db_proxy());

  auto fresh_deploy = cache2.try_get_deployed_contract(contract_addr)();
  ASSERT_TRUE(fresh_deploy.has_value());
  EXPECT_NE(fresh_deploy.value(), nullptr);

  auto deployed_hash = sha256(*contract);
  auto fresh_contract = cache2.try_get_contract_by_hash(deployed_hash)();
  ASSERT_TRUE(fresh_contract.has_value());
  EXPECT_NE(fresh_contract.value(), nullptr);

  // The deployment entry back-points at the same cached contract entry the
  // direct hash lookup returns, and a native contract has no translation.
  auto fresh_deploy_ptr = cache2.try_get_deployed_contract(contract_addr)();
  ASSERT_TRUE(fresh_deploy_ptr.has_value());
  ASSERT_NE(fresh_deploy_ptr.value(), nullptr);
  EXPECT_EQ(&fresh_deploy_ptr.value()->entry->contract, fresh_contract.value());
  EXPECT_FALSE(fresh_deploy_ptr.value()->entry->translated.has_value());

  EXPECT_TRUE(get_db_proxy()
                  .close_viewing_snapshot(snap2->viewing_block_id)()
                  .has_value());
  EXPECT_TRUE(get_db_proxy()
                  .close_viewing_snapshot(snap1->viewing_block_id)()
                  .has_value());
}

TEST_F(DirectStalenessTest, FreshViewingSnapshotSeesNewDbTip)
{
  advance_block();

  auto snap1 = get_db_proxy().open_viewing_snapshot()();
  ASSERT_TRUE(snap1.has_value());
  StorageSnapshotCache cache1(snap1->viewing_block_id, get_db_proxy());

  auto warm_tip = cache1.try_get_db_tip()();
  ASSERT_TRUE(warm_tip.has_value());
  BlockHeight old_height = warm_tip.value()->value.proposal_header.block_height;

  advance_block();

  // Stale cache still returns old height.
  auto stale_tip = cache1.try_get_db_tip()();
  ASSERT_TRUE(stale_tip.has_value());
  EXPECT_EQ(stale_tip.value()->value.proposal_header.block_height, old_height);

  // Fresh viewing snapshot sees incremented height.
  auto snap2 = get_db_proxy().open_viewing_snapshot()();
  ASSERT_TRUE(snap2.has_value());
  StorageSnapshotCache cache2(snap2->viewing_block_id, get_db_proxy());

  auto fresh_tip = cache2.try_get_db_tip()();
  ASSERT_TRUE(fresh_tip.has_value());
  EXPECT_GT(fresh_tip.value()->value.proposal_header.block_height, old_height);

  EXPECT_TRUE(get_db_proxy()
                  .close_viewing_snapshot(snap2->viewing_block_id)()
                  .has_value());
  EXPECT_TRUE(get_db_proxy()
                  .close_viewing_snapshot(snap1->viewing_block_id)()
                  .has_value());
}

// Regression test for issue #2399.
//
// During validation, a root frame that manually reverts must still submit RCQ
// observation-only deltas so HogDb can reject forged proposal observations.  If
// that observation push fails with a system error (e.g. the block was
// invalidated by a committed sibling), try_commit_transaction() must abort the
// underlying HogDb transaction before returning -- exactly like the normal
// root-success lock-push failure path -- so the InvokeTX and its block
// reference are released rather than leaked open.
TEST_F(DirectStalenessTest,
       ManualRevertObservationPushSystemErrorAbortsInvokeTx)
{
  static constexpr uint8_t kForgedEntry = 0xA5;
  static constexpr TransactionID kTxId = 4242;
  constexpr ContractAddress kContractAddr = make_contract_address(0);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0,
              [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
                auto observed =
                    co_await ctx.handler().retrocausal_queue_consume(
                        NativeContractTestFramework::make_storage_key("rcq0"),
                        true);
                if (!observed.has_value()) {
                  co_return observed.error().to_invoke_error();
                }
                if (!observed.value().has_value() ||
                    observed.value().value().size() != 1 ||
                    observed.value().value()[0] != kForgedEntry) {
                  co_return InvokeError{InvokeErrorCode::Deterministic};
                }

                ctx.handler().set_return_buffer(ReturnBuffer{}, true);
                co_return InvokeStatus<void>{};
              });
  auto native_contract = std::make_unique<NativeContract>(std::move(fns));
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  NativeContext runtime_context(std::move(contract_map));

  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(kContractAddr, contract);
  setup_grant_storage_token(kContractAddr, 1'000'000'000);
  advance_block();

  BlockID block_a = get_block_number();
  BlockID parent_id = get_db_tip().id;
  constexpr BlockID kSiblingOffset = 1000;
  BlockID block_b = block_a + kSiblingOffset;
  auto create_block_b = get_coord_client().dbsched_create_snapshot(
      parent_id, block_b, BlockExecutionMode::kBlockExecutionModeValidation);
  ASSERT_EQ(create_block_b, DbSchedResult::DbSchedResultOK);

  NondeterministicProposalData forged;
  forged.nondeterministic_bit_count = 1;
  forged.nondeterministic_bits = {uint8_t{1}};
  forged.observed_queue_entry_sequence = {QueueEntry{kForgedEntry}};

  Transaction tx{
      .invocation =
          Invocation{.invoked_address = kContractAddr,
                     .entrypoint = entrypoint_from_method_id(0),
                     .gas_limit = std::numeric_limits<GasAmount>::max(),
                     .calldata = {}},
      // Non-expired uniquifier: this test drives a manual revert and a
      // staleness abort, not replay expiry, and a default (height-0) uniquifier
      // is now rejected as expired during initial validation.
      .uniquifier = make_new_uniquifier()};

  StorageSnapshotCache block_b_cache(block_b, get_db_proxy());
  EventServerProxy event_proxy(
      x23::HostPort{"localhost", get_event_exec_port()});
  SystemCallHandler handler(
      tx, block_b_cache,
      PerTransactionDatabaseProxy(get_db_proxy(), block_b, kTxId),
      PerTransactionEventServerProxy(event_proxy, block_b, kTxId),
      ValidationModeCtor{std::move(forged)}, runtime_context);

  ASSERT_TRUE(handler.root_invoke()().is_root_success())
      << "The validation proposal data should drive the contract into a "
         "manual root-frame revert";

  close_and_commit_block(block_a);

  auto stale_read = get_db_proxy().get_db_tip(block_b)();
  ASSERT_FALSE(stale_read.has_value());
  ASSERT_EQ(stale_read.error().code(), SystemErrorCode::Abandon);

  // The manual-revert observation push fails with a system error; the executor
  // surfaces it as ExecResultSystemError.
  EXPECT_EQ(handler.try_commit_transaction()(),
            ExecResult::ExecResultSystemError);

  // Because try_commit_transaction() aborted the HogDb transaction before
  // returning, the InvokeTX's block reference is released and the first abandon
  // removes the stale block immediately.
  EXPECT_EQ(get_coord_client().dbsched_abandon_snapshot(block_b),
            DbSchedResult::DbSchedResultOK);

  // With the InvokeTX already closed and block_b removed, a fresh proxy can no
  // longer find or abort the original (block_b, tx_id) transaction.  Before the
  // fix this abort succeeded, proving the InvokeTX had been leaked open.
  PerTransactionDatabaseProxy cleanup(get_db_proxy(), block_b, kTxId);
  EXPECT_FALSE(cleanup.abort_transaction()().has_value())
      << "manual-revert observation push system error left the InvokeTX open";

  EXPECT_EQ(get_coord_client().dbsched_abandon_snapshot(block_b),
            DbSchedResult::DbSchedResultError);
}

} // namespace hogd
