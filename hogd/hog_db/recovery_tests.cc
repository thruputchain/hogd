#include <cassert>
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>

#include "consensus.hh"
#include "contract.hh"
#include "db_conflict.hh"
#include "db_sched.hh"
#include "hog_exec_constants.hh"
#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/network_config_params.h"
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
#include "network_config.hh"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_cost.hh"
#include "storage_item.hh"
#include "types.hh"
#include "x23/cleanup.h"
#include "x23/serialize.h"
#include "x23/tests/death_test.h"

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

// #4176: a tampered but XDR-valid <DBTIP> can carry a noncanonical network
// config (parameter ids not a gapless prefix from 0). Constructing RPCContext
// over it must not trip NetworkConfigParams' canonical-form invariant; the
// noncanonical tip is meant to be rejected later by StartupValidation, not by a
// fatal abort during construction. (StartupValidation's own rejection is
// covered by startup_validation_test.cc's RejectsNonCanonicalNetworkConfig.)
TEST(RecoveryTests, RPCContextAcceptsRecoveredNonCanonicalNetworkConfig)
{
  ErrnoStatus<Directory> dir = Directory::open_temporary();
  ASSERT_TRUE(dir.has_value());

  constexpr std::size_t kHeapSize = 1024 * 1024 * 1024;
  {
    DBStatus<std::unique_ptr<Database>> db =
        Database::open(dir->get_path(), kHeapSize, 0);
    ASSERT_TRUE(db.has_value());
    HogDbDAO dao(RAMDB(std::move(*db)));

    DbTip tip{};
    tip.storage_long_hash = dao.compute_full_db_hash();
    tip.network_config.v(0);
    tip.network_config.v0().params.push_back(NetworkConfigParams::make_param(
        ConfigParamID::kConfigParamMaxCrossInvokeDepth, 256));
    ASSERT_FALSE(NetworkConfigParams::is_canonical(tip.network_config.v0()));
    ASSERT_TRUE(dao.write_dbtip(tip).has_value());
    ASSERT_TRUE(dao.commit().has_value());
  }

  DBStatus<std::unique_ptr<Database>> db =
      Database::open(dir->get_path(), kHeapSize, 0);
  ASSERT_TRUE(db.has_value());

  // Must not abort: construction recovers the tip and defers rejection.
  RPCContext ctx(HogDbDAO(RAMDB(std::move(*db))), nullptr);
  const DbTip recovered = ctx.get_tip();
  EXPECT_FALSE(
      NetworkConfigParams::is_canonical(recovered.network_config.v0()));
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
// 2. Loading RCQ metadata rejects live_entries above
//    kRetrocausalQueueLiveEntriesHardCap as kCorruption, so corrupt on-disk
//    state never reaches cost arithmetic.

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
      BlockExecutionMode::kBlockExecutionModeProposal,
      static_cast<uint32_t>(kMaxSavepointDepth));
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
    EXPECT_EQ(DbConflictCode::CapacityExceeded,
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

namespace {

// Persists `metadata` under a fixed RCQ key, reopens the database, and
// returns what the DAO makes of the recovered row.
HogDbStatus<RetrocausalQueueMetadata>
reopen_and_read_rcq_metadata(const RetrocausalQueueMetadata &metadata)
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
    EXPECT_TRUE(
        dao.write_retrocausal_queue_metadata(rcq_key, metadata).has_value());
    EXPECT_TRUE(dao.commit().has_value());
  }

  DBStatus<std::unique_ptr<Database>> db =
      Database::open(dir->get_path(), kDefaultHeapSize, 0);
  EXPECT_TRUE(db);
  auto dao = HogDbDAO(RAMDB(std::move(*db)));
  return dao.read_retrocausal_queue_metadata(rcq_key);
}

} // namespace

// Guard 2: metadata the RCQ lock could never have written is rejected as
// kCorruption on load, in every build (#2917, #2936, #2979, #2986).
TEST(RecoveryTests, RecoveredConsistentRCQMetadataAcceptedOnLoad)
{
  RetrocausalQueueMetadata metadata{};
  metadata.live_entries = 5;
  metadata.max_live_block_num = 3;
  metadata.consume_state.max_fully_consumed_block_number = 1;
  metadata.consume_state.partial_consumed_number = 2;
  auto recovered = reopen_and_read_rcq_metadata(metadata);
  ASSERT_TRUE(recovered.has_value());
  EXPECT_EQ(recovered->live_entries, 5);
  EXPECT_EQ(recovered->max_live_block_num, 3);
}

TEST(RecoveryTests, RecoveredOverCapRCQLiveEntriesRejectedOnLoad)
{
  RetrocausalQueueMetadata metadata{};
  metadata.live_entries = uint64_t{1} << 63;
  auto recovered = reopen_and_read_rcq_metadata(metadata);
  ASSERT_FALSE(recovered.has_value());
  EXPECT_EQ(HogDbCode::kCorruption, recovered.error().code());
}

TEST(RecoveryTests, RecoveredInvertedRCQHeadTailRejectedOnLoad)
{
  RetrocausalQueueMetadata metadata{};
  metadata.live_entries = 1;
  metadata.max_live_block_num = 0;
  metadata.consume_state.max_fully_consumed_block_number = 1;
  auto recovered = reopen_and_read_rcq_metadata(metadata);
  ASSERT_FALSE(recovered.has_value());
  EXPECT_EQ(HogDbCode::kCorruption, recovered.error().code());
}

TEST(RecoveryTests, RecoveredMaxRCQTailRejectedOnLoad)
{
  RetrocausalQueueMetadata metadata{};
  metadata.live_entries = 1;
  metadata.max_live_block_num = std::numeric_limits<uint64_t>::max();
  metadata.consume_state.max_fully_consumed_block_number = 1;
  auto recovered = reopen_and_read_rcq_metadata(metadata);
  ASSERT_FALSE(recovered.has_value());
  EXPECT_EQ(HogDbCode::kCorruption, recovered.error().code());
}

TEST(RecoveryTests, RecoveredDanglingRCQPartialCursorRejectedOnLoad)
{
  RetrocausalQueueMetadata metadata{};
  metadata.live_entries = 1;
  metadata.max_live_block_num = 1;
  metadata.consume_state.max_fully_consumed_block_number = 1;
  metadata.consume_state.partial_consumed_number = 1;
  auto recovered = reopen_and_read_rcq_metadata(metadata);
  ASSERT_FALSE(recovered.has_value());
  EXPECT_EQ(HogDbCode::kCorruption, recovered.error().code());
}

namespace {

// Persists `block` under the row key for `stored_as`, reopens the database,
// and returns what the DAO makes of the recovered row when asked for
// `stored_as`.
HogDbStatus<RetrocausalQueueBlock>
reopen_and_read_rcq_block(const RetrocausalQueueBlock &block,
                          uint64_t stored_as)
{
  ErrnoStatus<Directory> dir = Directory::open_temporary();
  constexpr static size_t kDefaultHeapSize = 1024 * 1024 * 1024;
  EXPECT_TRUE(dir.has_value());
  if (!dir.has_value())
    return HogDbError(HogDbCode::kInternal);

  ItemKey rcq_key(ContractAddress{7}, StorageKeyContractData{42});

  {
    DBStatus<std::unique_ptr<Database>> db =
        Database::open(dir->get_path(), kDefaultHeapSize, 0);
    EXPECT_TRUE(db);
    if (!db)
      return HogDbError(HogDbCode::kInternal);
    auto dao = HogDbDAO(RAMDB(std::move(*db)));
    auto block_key = make_retrocausal_queue_block_key(rcq_key, stored_as);
    auto buf = x23::serialize(block);
    EXPECT_TRUE(dao.get_ramdb()
                    .put(std::as_bytes(std::span(block_key)), 0,
                         std::as_bytes(std::span(buf)))
                    .has_value());
    EXPECT_TRUE(dao.commit().has_value());
  }

  DBStatus<std::unique_ptr<Database>> db =
      Database::open(dir->get_path(), kDefaultHeapSize, 0);
  EXPECT_TRUE(db);
  if (!db)
    return HogDbError(HogDbCode::kInternal);
  auto dao = HogDbDAO(RAMDB(std::move(*db)));
  return dao.read_retrocausal_queue_block(rcq_key, stored_as);
}

} // namespace

// A block row must sit under its own number and hold 1 to
// kRetrocausalQueueBlockEntriesMaxLen entries; the DAO rejects anything else
// as kCorruption on load (#2978).
TEST(RecoveryTests, RecoveredConsistentRCQBlockAcceptedOnLoad)
{
  RetrocausalQueueBlock block;
  block.block_number = 1;
  block.entries.push_back(QueueEntry{1});
  auto recovered = reopen_and_read_rcq_block(block, 1);
  ASSERT_TRUE(recovered.has_value());
  EXPECT_EQ(recovered->entries.size(), 1);
}

TEST(RecoveryTests, RecoveredMisfiledRCQBlockRejectedOnLoad)
{
  RetrocausalQueueBlock block;
  block.block_number = 2;
  block.entries.push_back(QueueEntry{1});
  auto recovered = reopen_and_read_rcq_block(block, 1);
  ASSERT_FALSE(recovered.has_value());
  EXPECT_EQ(HogDbCode::kCorruption, recovered.error().code());
}

TEST(RecoveryTests, RecoveredEmptyRCQBlockRejectedOnLoad)
{
  RetrocausalQueueBlock block;
  block.block_number = 1;
  auto recovered = reopen_and_read_rcq_block(block, 1);
  ASSERT_FALSE(recovered.has_value());
  EXPECT_EQ(HogDbCode::kCorruption, recovered.error().code());
}

TEST(RecoveryTests, RecoveredOversizedRCQBlockRejectedOnLoad)
{
  RetrocausalQueueBlock block;
  block.block_number = 1;
  for (size_t i = 0; i <= kRetrocausalQueueBlockEntriesMaxLen; ++i)
    block.entries.push_back(QueueEntry{1});
  auto recovered = reopen_and_read_rcq_block(block, 1);
  ASSERT_FALSE(recovered.has_value());
  EXPECT_EQ(HogDbCode::kCorruption, recovered.error().code());
}

namespace {

// Persists `hash` as the address metadata of a fixed address, reopens the
// database, and returns what the DAO makes of the recovered row.
HogDbStatus<DeployedContractHash>
reopen_and_read_address_metadata(const DeployedContractHash &hash)
{
  ErrnoStatus<Directory> dir = Directory::open_temporary();
  constexpr static size_t kDefaultHeapSize = 1024 * 1024 * 1024;
  EXPECT_TRUE(dir.has_value());
  if (!dir.has_value())
    return HogDbError(HogDbCode::kInternal);

  Address address = ContractAddress{7};

  {
    DBStatus<std::unique_ptr<Database>> db =
        Database::open(dir->get_path(), kDefaultHeapSize, 0);
    EXPECT_TRUE(db);
    if (!db)
      return HogDbError(HogDbCode::kInternal);
    auto dao = HogDbDAO(RAMDB(std::move(*db)));
    EXPECT_TRUE(dao.write_address_metadata(address, hash).has_value());
    EXPECT_TRUE(dao.commit().has_value());
  }

  DBStatus<std::unique_ptr<Database>> db =
      Database::open(dir->get_path(), kDefaultHeapSize, 0);
  EXPECT_TRUE(db);
  if (!db)
    return HogDbError(HogDbCode::kInternal);
  auto dao = HogDbDAO(RAMDB(std::move(*db)));
  return dao.read_address_metadata(address);
}

} // namespace

// XDR decodes an enum by cast, so a recovered contract_type outside the enum
// parses. The DAO rejects it as kCorruption on load (#2964).
TEST(RecoveryTests, RecoveredKnownContractTypeAcceptedOnLoad)
{
  auto recovered = reopen_and_read_address_metadata(DeployedContractHash{
      .hash = {}, .contract_size = 10, .contract_type = ContractType::kRv32});
  ASSERT_TRUE(recovered.has_value());
  EXPECT_EQ(recovered->contract_type, ContractType::kRv32);
}

TEST(RecoveryTests, RecoveredUnknownContractTypeRejectedOnLoad)
{
  auto recovered = reopen_and_read_address_metadata(
      DeployedContractHash{.hash = {},
                           .contract_size = 10,
                           .contract_type = static_cast<ContractType>(0)});
  ASSERT_FALSE(recovered.has_value());
  EXPECT_EQ(HogDbCode::kCorruption, recovered.error().code());
}

// First touch prices the key through the DAO; corrupt state fail-stops
// there with the key in the message.
TEST(RecoveryTests, RecoveredCorruptRCQMetadataDiesOnFirstTouch)
{
  ErrnoStatus<Directory> dir = Directory::open_temporary();
  constexpr static size_t kDefaultHeapSize = 1024 * 1024 * 1024;
  ASSERT_TRUE(dir.has_value());

  ItemKey rcq_key(ContractAddress{7}, StorageKeyContractData{42});

  {
    DBStatus<std::unique_ptr<Database>> db =
        Database::open(dir->get_path(), kDefaultHeapSize, 0);
    ASSERT_TRUE(db);
    auto dao = HogDbDAO(RAMDB(std::move(*db)));
    ASSERT_TRUE(
        dao.write_address_balance(rcq_key.get_address(), int128_t{1} << 40)
            .has_value());

    RetrocausalQueueMetadata metadata{};
    metadata.live_entries = 1;
    metadata.max_live_block_num = 0;
    metadata.consume_state.max_fully_consumed_block_number = 1;
    ASSERT_TRUE(
        dao.write_retrocausal_queue_metadata(rcq_key, metadata).has_value());
    ASSERT_TRUE(dao.commit().has_value());
  }

  DBStatus<std::unique_ptr<Database>> db =
      Database::open(dir->get_path(), kDefaultHeapSize, 0);
  ASSERT_TRUE(db);
  auto dao = HogDbDAO(RAMDB(std::move(*db)));

  AuthChangeSubscriptionManager subscription_manager;
  BackgroundCommitWorker background_commit_worker(subscription_manager,
                                                  nullptr);
  StorageCostModel cost_model = StorageCostModel::default_cost_model();
  PerThreadARC<BlockTX> block_tx(
      dao, cost_model, background_commit_worker, 1,
      BlockExecutionMode::kBlockExecutionModeProposal,
      static_cast<uint32_t>(kMaxSavepointDepth));
  ASSERT_TRUE(block_tx.try_acquire_reference());
  InvokeTX invoke_tx(block_tx);
  invoke_tx.create_savepoint();

  X23_EXPECT_DEATH(
      {
        RetrocausalQueueDelta delta;
        delta.insert.added_entries.push_back(QueueEntry{1});
        std::ignore =
            invoke_tx.request_retrocausal_queue(rcq_key, std::move(delta),
                                                std::to_underlying(
                                                    StorageDeltaFlags::None));
      },
      "corrupt persisted state under key");
}

TEST(RecoveryTests, DefaultWalletPersistsAcrossReopen)
{
  ErrnoStatus<Directory> dir = Directory::open_temporary();
  ASSERT_TRUE(dir.has_value());

  constexpr BlockTXID kBlock = 21;
  constexpr std::size_t kHeapSize = 1024 * 1024 * 1024;

  DeployedContractHash wallet{};
  wallet.hash.fill(0xCD);
  wallet.contract_size = 0;
  wallet.contract_type = ContractType::kNative;

  {
    DBStatus<std::unique_ptr<Database>> db =
        Database::open(dir->get_path(), kHeapSize, 0);
    ASSERT_TRUE(db.has_value());
    RPCContext ctx(HogDbDAO(RAMDB(std::move(*db))), nullptr);

    // Genesis-order operations: install the config, then commit the tip.
    // update_dbtip only overwrites id/value/hash, so the field must survive.
    ctx.set_default_wallet(wallet);
    ConsensusValue value{};
    value.proposal_header.block_height = 0;
    ASSERT_TRUE(ctx.update_dbtip(kBlock, std::move(value), StorageLongHash{})
                    .has_value());
    EXPECT_EQ(ctx.get_tip().default_wallet.hash, wallet.hash);
    ASSERT_TRUE(ctx.get_db().commit().has_value());
  }

  DBStatus<std::unique_ptr<Database>> db =
      Database::open(dir->get_path(), kHeapSize, 0);
  ASSERT_TRUE(db.has_value());
  RPCContext recovered(HogDbDAO(RAMDB(std::move(*db))), nullptr);
  EXPECT_EQ(recovered.get_tip().default_wallet.hash, wallet.hash);
  EXPECT_EQ(recovered.get_tip().default_wallet.contract_type,
            ContractType::kNative);
}

// Regression test for issue #3955: a CRC-valid recovered <DBTIP> row can carry
// block_height == INT64_MAX while preserving the genuine id and state hash,
// because <DBTIP> is global metadata excluded from the state hash and
// read_dbtip() does not bound the recovered height. Opening the successor
// block then evaluated block_height + 1 on signed BlockHeight -- signed
// overflow, i.e. UB. open_block_tx_locked() now refuses a tip whose successor
// height is not representable, returning kOverflow before inserting a BlockTX.
TEST(RecoveryTests, RecoveredMaxBlockHeightRejectsSnapshotCreation)
{
  ErrnoStatus<Directory> dir = Directory::open_temporary();
  ASSERT_TRUE(dir.has_value());

  constexpr BlockTXID kParentBlock = 42;
  constexpr BlockTXID kFreshBlock = 43;
  constexpr std::size_t kHeapSize = 1024 * 1024 * 1024;

  {
    DBStatus<std::unique_ptr<Database>> db =
        Database::open(dir->get_path(), kHeapSize, 0);
    ASSERT_TRUE(db.has_value());
    HogDbDAO dao(RAMDB(std::move(*db)));

    DbTip tip{};
    tip.id = kParentBlock;
    tip.storage_long_hash = dao.compute_full_db_hash();
    tip.value.proposal_header.block_height =
        std::numeric_limits<BlockHeight>::max();
    ASSERT_TRUE(dao.write_dbtip(tip).has_value());
    ASSERT_TRUE(dao.commit().has_value());
  }

  DBStatus<std::unique_ptr<Database>> db =
      Database::open(dir->get_path(), kHeapSize, 0);
  ASSERT_TRUE(db.has_value());
  RPCContext ctx(HogDbDAO(RAMDB(std::move(*db))), nullptr);

  const DbTip recovered_tip = ctx.get_tip();
  ASSERT_EQ(recovered_tip.id, kParentBlock);
  ASSERT_EQ(recovered_tip.value.proposal_header.block_height,
            std::numeric_limits<BlockHeight>::max());

  // Opening the successor block must fail cleanly rather than overflow signed
  // BlockHeight, and must not insert the fresh block.
  auto result =
      ctx.open_block_tx(kFreshBlock,
                        BlockExecutionMode::kBlockExecutionModeProposal);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), HogDbCode::kOverflow);
  EXPECT_EQ(ctx.get_block_tx(kFreshBlock, false), nullptr);
}

} // namespace hogd
