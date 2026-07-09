#include <cassert>
#include <gtest/gtest.h>

#include <memory>
#include <span>
#include <utility>

#include "db_conflict.hh"
#include "db_sched.hh"
#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/perthread_arc.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogdb/auth_change_subscription_manager.h"
#include "hogd/hogdb/background_commit_worker.h"
#include "hogd/hogdb/block_tx.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/invoke_tx.h"
#include "hogd/hogdb/item_lock/retrocausal_queue_lock.h"
#include "hogd/hogdb/rpc_context.h"
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

TEST(RecoveryTests, RecoveryVaryingItems)
{
  ErrnoStatus<Directory> dir = Directory::open_temporary();
  constexpr static size_t kDefaultHeapSize = 1024 * 1024 * 1024;

  ASSERT_TRUE(dir.has_value());
  DbTip initial_tip{};
  DbHash db_hash;
  {
    DBStatus<std::unique_ptr<Database>> db =
        Database::open(dir->get_path(), kDefaultHeapSize, 0);
    ASSERT_TRUE(db);
    auto dao = HogDBDAO(RAMDB(std::move(*db)));
    EXPECT_EQ(dao.compute_full_db_hash(), DbHash{});

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
    auto hash_result = btx->get().close(ctx.get_tip().db_hash);
    ASSERT_TRUE(hash_result.has_value());
    db_hash = *hash_result;
    ASSERT_TRUE((btx->get()).commit().has_value());
    ASSERT_TRUE(
        ctx.update_dbtip(1, /*block_hash=*/{}, /*events_hash=*/{}, db_hash)
            .has_value());

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
  auto dao = HogDBDAO(RAMDB(std::move(*db)));
  EXPECT_EQ(db_hash, dao.compute_full_db_hash());
  ASSERT_EQ(db_hash, initial_tip.db_hash);

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
// 1. Loading RCQ metadata CHECKs that live_entries stays at or below
//    kMaxRetrocausalQueueLiveEntries, so corrupt on-disk state fail-stops
//    instead of reaching cost arithmetic.
// 2. Inserts are rejected once the queue is within one block of the cap, so
//    no legitimate history can ever produce an over-cap live_entries.

// Guard 2: a maximal-but-valid queue rejects further inserts up front.
TEST(RecoveryTests, RecoveredNearMaxRCQLiveEntriesRejectsInsert)
{
  ErrnoStatus<Directory> dir = Directory::open_temporary();
  constexpr static size_t kDefaultHeapSize = 1024 * 1024 * 1024;
  constexpr uint64_t kNearMaxLiveEntries = kMaxRetrocausalQueueLiveEntries - 10;

  ASSERT_TRUE(dir.has_value());

  ItemKey rcq_key(ContractAddress{7}, StorageKeyContractData{42});

  {
    DBStatus<std::unique_ptr<Database>> db =
        Database::open(dir->get_path(), kDefaultHeapSize, 0);
    ASSERT_TRUE(db);
    auto dao = HogDBDAO(RAMDB(std::move(*db)));

    ASSERT_TRUE(
        dao.write_address_balance(rcq_key.get_address(), 100'000).has_value());

    RetrocausalQueueMetadata metadata{};
    metadata.live_entries = kNearMaxLiveEntries;
    ASSERT_TRUE(
        dao.write_retrocausal_queue_metadata(rcq_key, metadata).has_value());
    ASSERT_TRUE(dao.commit().has_value());
  }

  DBStatus<std::unique_ptr<Database>> db =
      Database::open(dir->get_path(), kDefaultHeapSize, 0);
  ASSERT_TRUE(db);
  auto dao = HogDBDAO(RAMDB(std::move(*db)));

  // The near-max value is within the valid domain and survives recovery.
  auto recovered = dao.read_retrocausal_queue_metadata(rcq_key);
  ASSERT_TRUE(recovered.has_value());
  ASSERT_EQ(recovered->live_entries, kNearMaxLiveEntries);

  AuthChangeSubscriptionManager subscription_manager;
  BackgroundCommitWorker background_commit_worker(subscription_manager,
                                                  nullptr);
  StorageCostModel cost_model = StorageCostModel::default_cost_model();
  PerThreadARC<BlockTX> block_tx(
      dao, cost_model, background_commit_worker, 1,
      BlockExecutionMode::kBlockExecutionModeProposal);
  ASSERT_TRUE(block_tx.try_acquire_reference());
  InvokeTX invoke_tx(block_tx);
  invoke_tx.create_savepoint();

  RetrocausalQueueDelta delta;
  delta.insert.added_entries.push_back(QueueEntry{1});

  auto insert_result = invoke_tx.request_retrocausal_queue(
      rcq_key, std::move(delta), std::to_underlying(StorageDeltaFlags::None));
  ASSERT_FALSE(insert_result.has_value());
  EXPECT_EQ(HogDBCode::kConflict, insert_result.error().code());
  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  ASSERT_TRUE(invoke_tx.last_conflict_reason().has_value());
  EXPECT_EQ(DbConflictCode::kDbConflictCapacityExceeded,
            *invoke_tx.last_conflict_reason());
  // NOLINTEND(bugprone-unchecked-optional-access)

  invoke_tx.rollback_to_savepoint(0);
  invoke_tx.release_blocktx_reference();
}

// Guard 1: an over-cap live_entries value (only producible by on-disk
// corruption, since guard 2 bounds all legitimate growth) fail-stops the
// moment the metadata is loaded, before any signed cost arithmetic.
TEST(RecoveryTests, RecoveredHighBitRCQLiveEntriesDiesOnLoad)
{
  ErrnoStatus<Directory> dir = Directory::open_temporary();
  constexpr static size_t kDefaultHeapSize = 1024 * 1024 * 1024;
  constexpr uint64_t kHighBitLiveEntries = uint64_t{1} << 63;

  ASSERT_TRUE(dir.has_value());

  ItemKey rcq_key(ContractAddress{7}, StorageKeyContractData{42});

  {
    DBStatus<std::unique_ptr<Database>> db =
        Database::open(dir->get_path(), kDefaultHeapSize, 0);
    ASSERT_TRUE(db);
    auto dao = HogDBDAO(RAMDB(std::move(*db)));

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
        auto dao = HogDBDAO(RAMDB(std::move(*db)));
        auto recovered = dao.read_retrocausal_queue_metadata(rcq_key);
        (void)recovered;
      },
      "CHECK FAILED");
}

} // namespace hogd
