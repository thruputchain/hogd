// Diagnostics tests for HogDbExecutorRpc::try_push_new_subtransaction (#2759).

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <utility>

#include "contract.hh"
#include "db_conflict.hh"
#include "executor_to_db_rpc.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"

#include "hogd/base/compiler.h"
#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_db/invoke_tx.h"
#include "hogd/hog_db/rpc_context.h"
#include "hogd/hog_db/rpc_from_exec.h"
#include "hogd/ramdb/db.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/storage_cost/storage_cost_model.h"

namespace hogd {

namespace {

constexpr std::size_t kTestRamBytes = 64UL * 1024 * 1024;

HogDbDAO
make_dao()
{
  ErrnoStatus<Directory> dir = Directory::open_temporary();
  CHECK(dir.has_value());
  DBStatus<std::unique_ptr<Database>> db =
      Database::open(dir->get_path(),
                     Database::estimate_heap_size(kTestRamBytes), 0);
  CHECK(db.has_value());
  return HogDbDAO(RAMDB(std::move(*db)));
}

AddressAndKey
make_data_key(ContractAddress const &addr, std::uint8_t tag)
{
  StorageKey key(StorageKeyType::ContractData);
  key.contract_data_key()[0] = tag;
  return AddressAndKey{.address = addr, .key = std::move(key)};
}

FlaggedStorageDelta
make_bytes_delta(AddressAndKey addrkey, std::uint8_t byte)
{
  StorageDelta delta(StorageDeltaType::kDeltaTypeBytestringSet);
  delta.bytestring().push_back(byte);
  return FlaggedStorageDelta{.addrkey = std::move(addrkey),
                             .delta = std::move(delta),
                             .flags =
                                 std::to_underlying(StorageDeltaFlags::None)};
}

AddressAndKey
make_balance_key(ContractAddress const &addr)
{
  return AddressAndKey{.address = addr,
                       .key =
                           StorageKey(StorageKeyType::FreeNativeTokenBalance)};
}

FlaggedStorageDelta
make_balance_delta(ContractAddress const &addr, int128_t amount)
{
  StorageDelta delta(StorageDeltaType::kDeltaTypeNonnegativeIntAdd);
  delta.nnint_add() = to_xdr(amount);
  return FlaggedStorageDelta{.addrkey = make_balance_key(addr),
                             .delta = std::move(delta),
                             .flags =
                                 std::to_underlying(StorageDeltaFlags::None)};
}

// Requests to consume [uniquifier] for [addr]. The request itself succeeds
// (a fresh replay-cache entry); it's the balance lock it charges storage
// cost against that can later fail at sync_balance_locks.
FlaggedStorageDelta
make_consume_uniquifier_delta(ContractAddress const &addr,
                              Uniquifier uniquifier)
{
  StorageDelta delta(StorageDeltaType::kDeltaTypeConsumeUniquifier);
  delta.consume_uniquifier_args().uniquifier = uniquifier;
  return FlaggedStorageDelta{
      .addrkey = AddressAndKey{.address = addr,
                               .key = StorageKey(StorageKeyType::ReplayCache)},
      .delta = std::move(delta),
      .flags = std::to_underlying(StorageDeltaFlags::None)};
}

FlaggedStorageDelta
make_deploy_delta(ContractAddress const &addr, std::uint8_t hash_byte,
                  StorageDeltaFlagSet flags)
{
  StorageDelta delta(StorageDeltaType::kDeltaTypeDeployContract);
  DeployedContractHash hash{};
  hash.hash.fill(hash_byte);
  hash.contract_size = 32;
  hash.contract_type = ContractType::kWasmi;
  delta.deployed_contract_hash() = hash;
  return FlaggedStorageDelta{
      .addrkey = AddressAndKey{.address = addr,
                               .key = StorageKey(StorageKeyType::Metadata)},
      .delta = std::move(delta),
      .flags = flags};
}

class RpcFromExecDiagnosticsTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    HogDbDAO dao = make_dao();
    for (std::size_t i = 0; i < addr_.size(); ++i)
      addr_[i] = static_cast<std::uint8_t>(i + 1);
    ASSERT_TRUE(dao.write_address_balance(addr_, int128_t{1'000'000'000}));
    ctx_ = std::make_unique<RPCContext>(std::move(dao), nullptr);
    ctx_->initialize_cost_model(StorageCostModel::default_cost_model());
    ASSERT_TRUE(
        ctx_->open_block_tx(BlockID{1},
                            BlockExecutionMode::kBlockExecutionModeProposal));
    rpc_ = std::make_unique<HogDbExecutorRpc>(*ctx_);
  }

  // Pushes a winning write at [key] from [tx] in [block] so a differing
  // write at the same key conflicts.
  void seed_conflict(BlockID block, TransactionID tx, AddressAndKey const &key)
  {
    StorageDeltaBatch batch;
    batch.push_back(make_bytes_delta(key, 0xAA));
    auto r = rpc_->try_push_new_subtransaction(block, tx, batch);
    ASSERT_EQ(r.error(), RPCError::kRPCNone);
  }

  ContractAddress addr_{};
  std::unique_ptr<RPCContext> ctx_;
  std::unique_ptr<HogDbExecutorRpc> rpc_;
};

TEST_F(RpcFromExecDiagnosticsTest, ValidationModeReturnsNoDiagnostics)
{
  ASSERT_TRUE(
      ctx_->open_block_tx(BlockID{2},
                          BlockExecutionMode::kBlockExecutionModeValidation));

  auto key = make_data_key(addr_, 1);
  seed_conflict(BlockID{2}, TransactionID{1}, key);

  StorageDeltaBatch batch;
  batch.push_back(make_bytes_delta(key, 0xBB));
  auto r =
      rpc_->try_push_new_subtransaction(BlockID{2}, TransactionID{2}, batch);
  ASSERT_EQ(r.error(), RPCError::kRPCStorageWriteFailed);
  EXPECT_EQ(r.conflict().level(), DbDiagnosticLevel::kDbDiagnosticNone);
}

TEST_F(RpcFromExecDiagnosticsTest, ProposalModeReportsConflicts)
{
  auto key_a = make_data_key(addr_, 1);
  auto key_b = make_data_key(addr_, 2);
  seed_conflict(BlockID{1}, TransactionID{1}, key_a);
  {
    // Second winner, separate tx, second key.
    StorageDeltaBatch batch;
    batch.push_back(make_bytes_delta(key_b, 0xAA));
    auto r =
        rpc_->try_push_new_subtransaction(BlockID{1}, TransactionID{2}, batch);
    ASSERT_EQ(r.error(), RPCError::kRPCNone);
  }

  StorageDeltaBatch batch;
  batch.push_back(make_bytes_delta(key_a, 0xBB));
  batch.push_back(make_bytes_delta(key_b, 0xBB));
  auto r =
      rpc_->try_push_new_subtransaction(BlockID{1}, TransactionID{3}, batch);
  ASSERT_EQ(r.error(), RPCError::kRPCStorageWriteFailed);
  if constexpr (is_debug_build()) {
    ASSERT_EQ(r.conflict().level(), DbDiagnosticLevel::kDbDiagnosticAll);
    ASSERT_EQ(r.conflict().conflicts().size(), 2U);
    // key_a then key_b, both kDbConflictValueMismatch (order: hog_db
    // preserves batch order).
    EXPECT_EQ(r.conflict().conflicts()[0].addrkey, key_a);
    EXPECT_EQ(r.conflict().conflicts()[1].addrkey, key_b);
    for (auto const &d : r.conflict().conflicts()) {
      EXPECT_EQ(d.code, DbConflictCode::kDbConflictValueMismatch);
    }
  }
  else {
    ASSERT_EQ(r.conflict().level(), DbDiagnosticLevel::kDbDiagnosticFirst);
    EXPECT_EQ(r.conflict().first().addrkey, key_a);
    EXPECT_EQ(r.conflict().first().code,
              DbConflictCode::kDbConflictValueMismatch);
  }
}

TEST_F(RpcFromExecDiagnosticsTest, ProposalModeReportsAllBalanceConflicts)
{
  // Neither address below has a seeded balance (unlike addr_ from SetUp), so
  // consuming a uniquifier locks a positive storage cost against a balance
  // delta that starts at zero, driving it negative and failing bounds at
  // sync -- for both addresses, since the per-delta requests themselves
  // succeed and only balance sync fails.
  ContractAddress addr_a{};
  ContractAddress addr_b{};
  for (std::size_t i = 0; i < addr_a.size(); ++i) {
    addr_a[i] = static_cast<std::uint8_t>(0x10 + i);
    addr_b[i] = static_cast<std::uint8_t>(0x30 + i);
  }

  StorageDeltaBatch batch;
  batch.push_back(make_consume_uniquifier_delta(
      addr_a, Uniquifier{.block_expiry_height = BlockHeight{100}, .nonce = 1}));
  batch.push_back(make_consume_uniquifier_delta(
      addr_b, Uniquifier{.block_expiry_height = BlockHeight{100}, .nonce = 2}));

  auto r =
      rpc_->try_push_new_subtransaction(BlockID{1}, TransactionID{5}, batch);
  ASSERT_EQ(r.error(), RPCError::kRPCStorageWriteFailed);

  if constexpr (is_debug_build()) {
    ASSERT_EQ(r.conflict().level(), DbDiagnosticLevel::kDbDiagnosticAll);
    ASSERT_EQ(r.conflict().conflicts().size(), 2U);
    std::set<ContractAddress> expected{addr_a, addr_b};
    std::set<ContractAddress> actual;
    for (auto const &d : r.conflict().conflicts()) {
      EXPECT_EQ(d.code, DbConflictCode::kDbConflictBoundsExceeded);
      EXPECT_EQ(d.addrkey.key.key_type(),
                StorageKeyType::FreeNativeTokenBalance);
      actual.insert(d.addrkey.address);
    }
    EXPECT_EQ(expected, actual);
  }
  else {
    ASSERT_EQ(r.conflict().level(), DbDiagnosticLevel::kDbDiagnosticFirst);
    EXPECT_EQ(r.conflict().first().code,
              DbConflictCode::kDbConflictBoundsExceeded);
    EXPECT_EQ(r.conflict().first().addrkey.key.key_type(),
              StorageKeyType::FreeNativeTokenBalance);
    EXPECT_TRUE(r.conflict().first().addrkey.address == addr_a ||
                r.conflict().first().addrkey.address == addr_b);
  }
}

TEST_F(RpcFromExecDiagnosticsTest, FailedPushRollsBackRegardlessOfLevel)
{
  auto key_a = make_data_key(addr_, 1);
  auto key_b = make_data_key(addr_, 2);
  seed_conflict(BlockID{1}, TransactionID{1}, key_a);

  // Tx 3: one good delta (key_b) + one conflicting delta (key_a).
  StorageDeltaBatch batch;
  batch.push_back(make_bytes_delta(key_b, 0xCC));
  batch.push_back(make_bytes_delta(key_a, 0xBB));
  auto r =
      rpc_->try_push_new_subtransaction(BlockID{1}, TransactionID{3}, batch);
  ASSERT_EQ(r.error(), RPCError::kRPCStorageWriteFailed);

  // The whole subtransaction rolled back: tx 4 can now claim key_b with
  // different bytes (no residue from tx 3's successful key_b delta).
  StorageDeltaBatch retry;
  retry.push_back(make_bytes_delta(key_b, 0xDD));
  auto r2 =
      rpc_->try_push_new_subtransaction(BlockID{1}, TransactionID{4}, retry);
  EXPECT_EQ(r2.error(), RPCError::kRPCNone);
}

// A failed push must leave the root InvokeTX holding nothing for the keys
// only the failed child touched. Retained empty entries are invisible to
// commit/abort/sync, but rollback skips empty vectors, so they would survive
// every later rollback -- letting a contract that repeats a failing TryInvoke
// against fresh addresses grow the live transaction's maps (#3090).
TEST_F(RpcFromExecDiagnosticsTest, FailedBalancePushRetainsNoEntries)
{
  // Under the cost model's balance cost, so crediting a fresh receiver never
  // pays for its own balance row: its lock is rejected on first admission in
  // update_balance_lock(), before any entry can be published.
  constexpr int128_t kTransferAmount = 100;
  ASSERT_LT(kTransferAmount,
            StorageCostModel::default_cost_model().get_balance_cost());

  constexpr int kAttempts = 8;
  for (int i = 0; i < kAttempts; ++i) {
    ContractAddress receiver{};
    for (std::size_t j = 0; j < receiver.size(); ++j)
      receiver[j] = static_cast<std::uint8_t>(0xB0 + j);
    receiver[0] = static_cast<std::uint8_t>(i);

    // addr_ is funded, so the debit locks successfully and is then rolled
    // back; the fresh receiver's credit is what fails.
    StorageDeltaBatch batch;
    batch.push_back(make_balance_delta(addr_, -kTransferAmount));
    batch.push_back(make_balance_delta(receiver, kTransferAmount));

    auto r =
        rpc_->try_push_new_subtransaction(BlockID{1}, TransactionID{9}, batch);
    ASSERT_EQ(r.error(), RPCError::kRPCStorageWriteFailed) << i;

    InvokeTX *itx = ctx_->get_invoke_tx(BlockID{1}, TransactionID{9});
    ASSERT_NE(itx, nullptr);
    EXPECT_EQ(itx->get_savepoint_head(), 0U) << i;
    EXPECT_EQ(itx->change_entry_count_for_testing(), 0U) << i;
    EXPECT_EQ(itx->balance_entry_count_for_testing(), 0U) << i;
  }
}

// Same invariant for ordinary storage keys: the successful key_b write and
// the storage cost it locked against addr_'s balance are both rolled back,
// and neither leaves an entry behind.
TEST_F(RpcFromExecDiagnosticsTest, FailedDataPushRetainsNoEntries)
{
  auto key_a = make_data_key(addr_, 1);
  auto key_b = make_data_key(addr_, 2);
  seed_conflict(BlockID{1}, TransactionID{1}, key_a);

  StorageDeltaBatch batch;
  batch.push_back(make_bytes_delta(key_b, 0xCC));
  batch.push_back(make_bytes_delta(key_a, 0xBB));
  auto r =
      rpc_->try_push_new_subtransaction(BlockID{1}, TransactionID{3}, batch);
  ASSERT_EQ(r.error(), RPCError::kRPCStorageWriteFailed);

  InvokeTX *itx = ctx_->get_invoke_tx(BlockID{1}, TransactionID{3});
  ASSERT_NE(itx, nullptr);
  EXPECT_EQ(itx->get_savepoint_head(), 0U);
  EXPECT_EQ(itx->change_entry_count_for_testing(), 0U);
  EXPECT_EQ(itx->balance_entry_count_for_testing(), 0U);
}

// Regression test for #1189: request_one dropped [flags] on the floor for
// kDeltaTypeDeployContract deltas, so an exclusive lock requested on a deploy
// was silently downgraded to an ordinary reservation -- MockDB blocked the
// second deploy, hog_db did not. The two tests below pin both halves: the
// flag now blocks, and its absence still admits an identical concurrent
// deploy (so the block above comes from the flag, not from the deploy path
// having become conflict-happy).
TEST_F(RpcFromExecDiagnosticsTest, DeployContractExclusiveFlagBlocksConcurrent)
{
  StorageDeltaBatch exclusive;
  exclusive.push_back(
      make_deploy_delta(addr_, 0xD1,
                        std::to_underlying(StorageDeltaFlags::ExclusiveLock)));
  auto r1 = rpc_->try_push_new_subtransaction(BlockID{1}, TransactionID{1},
                                              exclusive);
  ASSERT_EQ(r1.error(), RPCError::kRPCNone);

  // Same address, same contract hash: without the exclusive lock this would
  // be admitted alongside (see the control test below).
  StorageDeltaBatch concurrent;
  concurrent.push_back(
      make_deploy_delta(addr_, 0xD1,
                        std::to_underlying(StorageDeltaFlags::None)));
  auto r2 = rpc_->try_push_new_subtransaction(BlockID{1}, TransactionID{2},
                                              concurrent);
  ASSERT_EQ(r2.error(), RPCError::kRPCStorageWriteFailed);

  if constexpr (is_debug_build()) {
    ASSERT_EQ(r2.conflict().level(), DbDiagnosticLevel::kDbDiagnosticAll);
    ASSERT_EQ(r2.conflict().conflicts().size(), 1U);
    EXPECT_EQ(r2.conflict().conflicts()[0].addrkey.key.key_type(),
              StorageKeyType::Metadata);
    EXPECT_EQ(r2.conflict().conflicts()[0].code,
              DbConflictCode::kDbConflictExclusiveLock);
  }
  else {
    ASSERT_EQ(r2.conflict().level(), DbDiagnosticLevel::kDbDiagnosticFirst);
    EXPECT_EQ(r2.conflict().first().addrkey.key.key_type(),
              StorageKeyType::Metadata);
    EXPECT_EQ(r2.conflict().first().code,
              DbConflictCode::kDbConflictExclusiveLock);
  }
}

TEST_F(RpcFromExecDiagnosticsTest, DeployContractWithoutFlagAdmitsConcurrent)
{
  StorageDeltaBatch first;
  first.push_back(
      make_deploy_delta(addr_, 0xD2,
                        std::to_underlying(StorageDeltaFlags::None)));
  auto r1 =
      rpc_->try_push_new_subtransaction(BlockID{1}, TransactionID{1}, first);
  ASSERT_EQ(r1.error(), RPCError::kRPCNone);

  StorageDeltaBatch second;
  second.push_back(
      make_deploy_delta(addr_, 0xD2,
                        std::to_underlying(StorageDeltaFlags::None)));
  auto r2 =
      rpc_->try_push_new_subtransaction(BlockID{1}, TransactionID{2}, second);
  EXPECT_EQ(r2.error(), RPCError::kRPCNone);
}

} // namespace

} // namespace hogd
