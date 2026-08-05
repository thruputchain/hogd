#include <cassert>
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

#include "consensus.hh"
#include "db_conflict.hh"
#include "db_sched.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/perthread_arc.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hog_db/auth_change_subscription_manager.h"
#include "hogd/hog_db/background_commit_worker.h"
#include "hogd/hog_db/block_tx.h"
#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_db/hog_types.h"
#include "hogd/hog_db/invoke_tx.h"
#include "hogd/hog_db/item_lock/retrocausal_queue_lock.h"
#include "hogd/hog_db/rpc_context.h"
#include "hogd/hog_db/status.h"
#include "hogd/ramdb/db.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_cost.hh"
#include "storage_item.hh"
#include "types.hh"
#include "x23/cleanup.h"

namespace hogd {

namespace {

HogAge
make_hog_age(uint128_t num_hogs, BlockHeight average_age)
{
  auto num_hogs_xdr = to_xdr(num_hogs);
  return HogAge{.address = ContractAddress{},
                .num_hogs = {{num_hogs_xdr, num_hogs_xdr, num_hogs_xdr}},
                .average_ages = {{average_age, average_age, average_age}}};
}

} // namespace

TEST(RecoveryTests, RPCContextAcceptsMissingDbTipAsFresh)
{
  ErrnoStatus<Directory> dir = Directory::open_temporary();
  ASSERT_TRUE(dir.has_value());

  DBStatus<std::unique_ptr<Database>> db =
      Database::open(dir->get_path(), 1024 * 1024 * 1024, 0);
  ASSERT_TRUE(db.has_value());

  RPCContext ctx(HogDbDAO(RAMDB(std::move(*db))), nullptr);
  EXPECT_FALSE(ctx.has_committed_tip());
  EXPECT_EQ(ctx.get_tip().storage_long_hash, StorageLongHash{});
}

TEST(RecoveryTests, RPCContextRejectsNonNotFoundDbTipError)
{
  ErrnoStatus<Directory> dir = Directory::open_temporary();
  ASSERT_TRUE(dir.has_value());

  DBStatus<std::unique_ptr<Database>> db =
      Database::open(dir->get_path(), 1024 * 1024 * 1024, 0);
  ASSERT_TRUE(db.has_value());
  HogDbDAO dao(RAMDB(std::move(*db)));

  constexpr std::string_view kDbTipKey = "<DBTIP";
  constexpr uint8_t kWrongType = 1;
  std::array<std::byte, 1> value{};
  ASSERT_TRUE(
      dao.get_ramdb()
          .put(std::as_bytes(std::span(kDbTipKey.data(), kDbTipKey.size())),
               kWrongType, value)
          .has_value());

  HogDbStatus<DbTip> tip = dao.read_dbtip();
  ASSERT_FALSE(tip.has_value());
  ASSERT_EQ(tip.error().code(), HogDbCode::kTypeMismatch);
  EXPECT_THROW((void)RPCContext(std::move(dao), nullptr), std::system_error);
}

TEST(RecoveryTests, SnapshotStalenessTracksScalarCommittedTip)
{
  ErrnoStatus<Directory> dir = Directory::open_temporary();
  ASSERT_TRUE(dir.has_value());

  constexpr BlockTXID kInstalledBlock = 16;
  constexpr BlockTXID kFirstBlock = 17;
  constexpr BlockTXID kSecondBlock = 18;
  constexpr std::size_t kHeapSize = 1024 * 1024 * 1024;

  {
    DBStatus<std::unique_ptr<Database>> db =
        Database::open(dir->get_path(), kHeapSize, 0);
    ASSERT_TRUE(db.has_value());

    RPCContext ctx(HogDbDAO(RAMDB(std::move(*db))), nullptr);

    EXPECT_TRUE(ctx.is_snapshot_read_stale(kFirstBlock));

    ConsensusValue installed_value{};
    installed_value.proposal_header.block_height = 0;
    ctx.set_tip(DbTip{.id = kInstalledBlock,
                      .storage_long_hash = {},
                      .value = std::move(installed_value)});
    EXPECT_FALSE(ctx.is_snapshot_read_stale(kInstalledBlock));
    EXPECT_TRUE(ctx.is_snapshot_read_stale(kFirstBlock));

    ConsensusValue first_value{};
    first_value.proposal_header.block_height = 1;
    ASSERT_TRUE(
        ctx.update_dbtip(kFirstBlock, std::move(first_value), StorageLongHash{})
            .has_value());
    EXPECT_FALSE(ctx.is_snapshot_read_stale(kFirstBlock));

    ConsensusValue second_value{};
    second_value.proposal_header.block_height = 2;
    ASSERT_TRUE(ctx.update_dbtip(kSecondBlock, std::move(second_value),
                                 StorageLongHash{})
                    .has_value());
    EXPECT_TRUE(ctx.is_snapshot_read_stale(kFirstBlock));
    EXPECT_FALSE(ctx.is_snapshot_read_stale(kSecondBlock));
    ASSERT_TRUE(ctx.get_db().commit().has_value());
  }

  DBStatus<std::unique_ptr<Database>> db =
      Database::open(dir->get_path(), kHeapSize, 0);
  ASSERT_TRUE(db.has_value());
  RPCContext recovered(HogDbDAO(RAMDB(std::move(*db))), nullptr);

  EXPECT_TRUE(recovered.is_snapshot_read_stale(kFirstBlock));
  EXPECT_FALSE(recovered.is_snapshot_read_stale(kSecondBlock));
  EXPECT_FALSE(recovered.is_committed_read_stale(recovered.get_tip()));
}

TEST(RecoveryTests, RecoveryVaryingItems)
{
  ErrnoStatus<Directory> dir = Directory::open_temporary();
  constexpr static size_t kDefaultHeapSize = 1024 * 1024 * 1024;

  ASSERT_TRUE(dir.has_value());
  DbTip initial_tip{};
  StorageLongHash db_hash;
  {
    DBStatus<std::unique_ptr<Database>> db =
        Database::open(dir->get_path(), kDefaultHeapSize, 0);
    ASSERT_TRUE(db);
    auto dao = HogDbDAO(RAMDB(std::move(*db)));
    EXPECT_EQ(dao.compute_full_db_hash(), StorageLongHash{});

    // add some items to the database
    // some items that are not in the db hash
    CHECK(dao.write_address_average_hog_age(make_hog_age(0U, 4)).has_value());
    // some items that are in the db hash

    RPCContext ctx(std::move(dao), nullptr);
    ctx.initialize_cost_model(StorageCostModel(StorageCostModelXdr{
        .ext = ExtensionPoint(0),
        .base_data_item_cost = 12000,
        .bytestring_cost_per_byte = 10,
        .base_contract_deployment_cost = 12000,
        .contract_deployment_cost_per_byte = 10,
    }));
    ASSERT_TRUE(
        ctx.get_db().write_cost_model(ctx.get_cost_model()).has_value());
    auto open_result =
        ctx.open_block_tx(1, BlockExecutionMode::kBlockExecutionModeProposal);
    ASSERT_TRUE(open_result.has_value());

    auto *itx = ctx.get_invoke_tx(1, 1);

    ASSERT_TRUE(itx);
    auto sp = itx->create_savepoint();
    auto cleanup =
        x23::Defer([itx, sp]() { itx->rollback_to_savepoint(sp - 1); });

    ASSERT_TRUE(
        itx->request_nnint128(make_address_balance_item_key(ContractAddress{0}),
                              ctx.get_cost_model().get_nnint_cost() +
                                  ctx.get_cost_model().get_bytestring_cost(6) +
                                  ctx.get_cost_model().get_balance_cost(),
                              std::to_underlying(StorageDeltaFlags::None))
            .has_value());
    ASSERT_TRUE(
        itx->request_nnint128(ItemKey(ContractAddress{0},
                                      StorageKeyContractData{0}),
                              10, std::to_underlying(StorageDeltaFlags::None))
            .has_value());
    ASSERT_TRUE(itx->request_bytes(ItemKey(ContractAddress{0},
                                           StorageKeyContractData{1}),
                                   std::as_bytes(std::span("hello")),
                                   std::to_underlying(StorageDeltaFlags::None))
                    .has_value());

    // invoketx assertions fail if you call the dtor in an invalid state (where
    // i.e requests failed).
    cleanup.release();
    ASSERT_TRUE(itx->sync_balance_locks().has_value());

    ASSERT_TRUE(itx->commit());
    ctx.close_invoke_tx(1, 1, itx);

    auto *btx = ctx.get_block_tx(1, false);
    assert(btx != nullptr);
    // close() stages changes and computes hash; must be called before commit().
    auto hash_result = btx->get().close(ctx.get_tip().storage_long_hash);
    ASSERT_TRUE(hash_result.has_value());
    db_hash = *hash_result;
    ASSERT_TRUE((btx->get()).commit().has_value());
    ConsensusValue value{};
    value.proposal_header.block_height = 1;
    ASSERT_TRUE(ctx.update_dbtip(1, value, db_hash).has_value());

    ASSERT_TRUE(btx->initiate_shutdown());
    ctx.remove_shutdown_block_tx(1);

    // commit to disk
    auto lsn = ctx.get_db().commit();
    ASSERT_TRUE(lsn.has_value());
    EXPECT_EQ(*lsn, 1UL);

    initial_tip = ctx.get_tip();
    EXPECT_EQ(db_hash, ctx.get_db().compute_full_db_hash());
  }

  // now reload the database
  DBStatus<std::unique_ptr<Database>> db =
      Database::open(dir->get_path(), kDefaultHeapSize, 0);
  ASSERT_TRUE(db);
  auto dao = HogDbDAO(RAMDB(std::move(*db)));
  EXPECT_EQ(db_hash, dao.compute_full_db_hash());
  ASSERT_EQ(db_hash, initial_tip.storage_long_hash);

  auto bal = dao.read_address_balance(ContractAddress{0});
  ASSERT_TRUE(bal.has_value());
  EXPECT_EQ(*bal, 0);

  ASSERT_TRUE(dao.read_cost_model().has_value());
}

// Regression tests for issue #2917: RCQ metadata persists live_entries as an
// unsigned hyper, but storage-cost accounting is signed 64-bit. A CRC-valid
// recovered row with the high bit set used to sail through recovery and RCQ
// admission, then die on CHECK_LE in InvokeTX::commit when the
// implementation-defined unsigned-to-signed conversion produced a cost change
// exceeding the storage reservation. Two guards close this off:
// 1. Inserts are banned once the committed queue size has reached
//    kMaxRetrocausalQueueLiveEntries; live_entries may overshoot by at most
//    one block, and a static_assert keeps the overshoot inside the signed
//    domain. No legitimate history can exceed the hard cap.
// 2. Loading RCQ metadata CHECKs live_entries against
//    kRetrocausalQueueLiveEntriesHardCap, so corrupt on-disk state
//    fail-stops instead of reaching cost arithmetic.

namespace {

// Shared harness for the insert-admission boundary tests: persists an RCQ
// whose committed live_entries is `live_entries`, reopens the database, and
// issues a single-entry insert against the recovered queue. Returns the
// result of the insert request, committing the transaction (and block) when
// admitted.
HogDbStatus<void>
insert_into_recovered_rcq(uint64_t live_entries)
{
  ErrnoStatus<Directory> dir = Directory::open_temporary();
  constexpr static size_t kDefaultHeapSize = 1024 * 1024 * 1024;

  EXPECT_TRUE(dir.has_value());

  ItemKey rcq_key(ContractAddress{7}, StorageKeyContractData{42});

  {
    DBStatus<std::unique_ptr<Database>> db =
        Database::open(dir->get_path(), kDefaultHeapSize, 0);
    EXPECT_TRUE(db);
    auto dao = HogDbDAO(RAMDB(std::move(*db)));

    // Enough to cover the near-int64-max storage reservation of a
    // maximal queue (~2^63 entries * 8 tokens each).
    EXPECT_TRUE(
        dao.write_address_balance(rcq_key.get_address(), int128_t{1} << 70)
            .has_value());

    RetrocausalQueueMetadata metadata{};
    metadata.live_entries = live_entries;
    EXPECT_TRUE(
        dao.write_retrocausal_queue_metadata(rcq_key, metadata).has_value());
    EXPECT_TRUE(dao.commit().has_value());
  }

  DBStatus<std::unique_ptr<Database>> db =
      Database::open(dir->get_path(), kDefaultHeapSize, 0);
  EXPECT_TRUE(db);
  auto dao = HogDbDAO(RAMDB(std::move(*db)));

  // The value is within the valid domain and survives recovery.
  auto recovered = dao.read_retrocausal_queue_metadata(rcq_key);
  EXPECT_TRUE(recovered.has_value());
  EXPECT_EQ(recovered->live_entries, live_entries);

  AuthChangeSubscriptionManager subscription_manager;
  BackgroundCommitWorker background_commit_worker(subscription_manager,
                                                  nullptr);
  StorageCostModel cost_model = StorageCostModel::default_cost_model();
  PerThreadARC<BlockTX> block_tx(
      dao, cost_model, background_commit_worker, 1,
      BlockExecutionMode::kBlockExecutionModeProposal);
  EXPECT_TRUE(block_tx.try_acquire_reference());
  InvokeTX invoke_tx(block_tx);
  invoke_tx.create_savepoint();

  RetrocausalQueueDelta delta;
  delta.insert.added_entries.push_back(QueueEntry{1});

  auto insert_result = invoke_tx.request_retrocausal_queue(
      rcq_key, std::move(delta), std::to_underlying(StorageDeltaFlags::None));

  if (!insert_result.has_value()) {
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    EXPECT_TRUE(invoke_tx.last_conflict_reason().has_value());
    EXPECT_EQ(DbConflictCode::kDbConflictCapacityExceeded,
              *invoke_tx.last_conflict_reason());
    // NOLINTEND(bugprone-unchecked-optional-access)
    invoke_tx.rollback_to_savepoint(0);
    return insert_result;
  }

  // Admitted: drive the change through commit accounting and block staging,
  // which exercises the signed cost arithmetic at the boundary.
  EXPECT_TRUE(invoke_tx.sync_balance_locks().has_value());
  EXPECT_TRUE(invoke_tx.commit());

  EXPECT_TRUE(block_tx.get().close(StorageLongHash{}).has_value());
  EXPECT_TRUE(block_tx.get().commit().has_value());

  auto updated = dao.read_retrocausal_queue_metadata(rcq_key);
  EXPECT_TRUE(updated.has_value());
  EXPECT_EQ(updated->live_entries, live_entries + 1);

  return insert_result;
}

} // namespace

// Guard 1, rejection side: a queue whose committed size is exactly at the
// bound refuses further inserts up front.
TEST(RecoveryTests, RCQInsertRejectedAtMaxLiveEntries)
{
  auto result = insert_into_recovered_rcq(kMaxRetrocausalQueueLiveEntries);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(HogDbCode::kConflict, result.error().code());
}

// Guard 1, admission side: one entry below the bound, the insert is admitted
// and commits cleanly through the signed cost arithmetic.
TEST(RecoveryTests, RCQInsertAdmittedJustBelowMaxLiveEntries)
{
  auto result = insert_into_recovered_rcq(kMaxRetrocausalQueueLiveEntries - 1);
  ASSERT_TRUE(result.has_value());
}

// Guard 2: an over-cap live_entries value (only producible by on-disk
// corruption, since guard 1 bounds all legitimate growth) fail-stops the
// moment the metadata is loaded, before any signed cost arithmetic.
TEST(RecoveryTests, RecoveredHighBitRCQLiveEntriesDiesOnLoad)
{
  if constexpr (!is_debug_build()) {
    // The "CHECK FAILED" panic message this death test matches is only
    // produced in debug builds (x23::panic is std::unreachable() under
    // NDEBUG).
    GTEST_SKIP();
  }

  ErrnoStatus<Directory> dir = Directory::open_temporary();
  constexpr static size_t kDefaultHeapSize = 1024 * 1024 * 1024;
  constexpr uint64_t kHighBitLiveEntries = uint64_t{1} << 63;

  ASSERT_TRUE(dir.has_value());

  ItemKey rcq_key(ContractAddress{7}, StorageKeyContractData{42});

  {
    DBStatus<std::unique_ptr<Database>> db =
        Database::open(dir->get_path(), kDefaultHeapSize, 0);
    ASSERT_TRUE(db);
    auto dao = HogDbDAO(RAMDB(std::move(*db)));

    RetrocausalQueueMetadata metadata{};
    metadata.live_entries = kHighBitLiveEntries;
    ASSERT_TRUE(
        dao.write_retrocausal_queue_metadata(rcq_key, metadata).has_value());
    ASSERT_TRUE(dao.commit().has_value());
  }

  EXPECT_DEATH(
      {
        DBStatus<std::unique_ptr<Database>> db =
            Database::open(dir->get_path(), kDefaultHeapSize, 0);
        ASSERT_TRUE(db);
        auto dao = HogDbDAO(RAMDB(std::move(*db)));
        auto recovered = dao.read_retrocausal_queue_metadata(rcq_key);
        (void)recovered;
      },
      "CHECK FAILED");
}

} // namespace hogd
