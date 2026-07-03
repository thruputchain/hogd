// Diagnostics tests for HogDBExecutorRpc::try_push_new_subtransaction (#2759).

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "db_conflict.hh"
#include "executor_to_db_rpc.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"

#include "hogd/base/compiler.h"
#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/rpc_context.h"
#include "hogd/hogdb/rpc_from_exec.h"
#include "hogd/ramdb/db.h"
#include "hogd/storage_cost/storage_cost_model.h"

namespace hogd {

namespace {

constexpr std::size_t kTestRamBytes = 64UL * 1024 * 1024;

HogDBDAO
make_dao()
{
  ErrnoStatus<Directory> dir = Directory::open_temporary();
  CHECK(dir.has_value());
  DBStatus<std::unique_ptr<Database>> db =
      Database::open(dir->get_path(),
                     Database::estimate_heap_size(kTestRamBytes), 0);
  CHECK(db.has_value());
  return HogDBDAO(RAMDB(std::move(*db)));
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

class RpcFromExecDiagnosticsTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    HogDBDAO dao = make_dao();
    for (std::size_t i = 0; i < addr_.size(); ++i)
      addr_[i] = static_cast<std::uint8_t>(i + 1);
    ASSERT_TRUE(dao.write_address_balance(addr_, int128_t{1'000'000'000}));
    ctx_ = std::make_unique<RPCContext>(std::move(dao), nullptr);
    ctx_->initialize_cost_model(StorageCostModel::default_cost_model());
    ASSERT_TRUE(
        ctx_->open_block_tx(BlockID{1},
                            BlockExecutionMode::kBlockExecutionModeProposal));
    rpc_ = std::make_unique<HogDBExecutorRpc>(*ctx_);
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
  std::unique_ptr<HogDBExecutorRpc> rpc_;
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
    // key_a then key_b, both kDbConflictValueMismatch (order: hogdb
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

} // namespace

} // namespace hogd
