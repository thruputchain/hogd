// Tolerated behavior (WAI): the permanent-exclusive bit is a property of the
// change a transaction actually commits, not a sticky property of the key.
//
// `StorageDeltaFlags::ExclusiveLock` is defined by the protocol as a flag on a
// *delta*: "forces the delta to acquire an exclusive lock on the item at the
// storage key" (protocol/storage_item.x).  A transaction that writes a key
// twice -- once with the flag, then again without it -- is not asking for the
// key to stay frozen; its second write superseded the first, flag included.
// Both layers implement that same last-write-wins rule:
//
//   - executor: `ItemView::set()` overwrites `flags_` on every write, so only
//     the final write's flags reach the database at all.
//   - hog_db:   `ItemChange::narrow()` merges the earlier reservation into the
//     later change, promoting it to ordinary `exclusive_`, but it does not
//     carry `permanent_exclusive_` forward.  `InvokeTX::request_change()` then
//     retains the narrowed change as the transaction's final version, and
//     `ItemLock::commit_change()` publishes `kFinalizedPermamentExclusiveLock`
//     only when that final version still carries the bit.
//
// Consequence, exercised below: an `ExclusiveLock` write followed by an
// unflagged write to the same key in the same `InvokeTX` finalizes as an
// ordinary `kFinalized` lock, so a later transaction in the same block can
// still modify the key.
//
// Why this is tolerated:
//   - No isolation the transaction could rely on is lost.  `narrow()` still
//     forces the merged reservation to ordinary-exclusive, so no *other*
//     transaction can interleave while the flagged write is pending, and a
//     rollback to the flagged version always succeeds (see
//     RollbackToSavepointRestoresPermanentExclusiveReservation).  The only
//     thing dropped is the post-commit freeze, and the only party that can
//     drop it is the writer itself, by superseding its own delta.
//   - Propagating the bit would be the actual defect: it would let a
//     transaction freeze a key against every other transaction for the rest of
//     the block from a delta that never requested it, purely as a side effect
//     of a superseded earlier write.
//   - Storage cost stays coupled to the request.  Only a permanent-exclusive
//     change may reserve a negative marginal cost (`snapshot_cost_transform()`
//     refunds the item's snapshot cost, since nothing else may re-grow the
//     item in this block).  Narrowing down rescinds that refund in the same
//     step that drops the freeze -- see
//     PermanentExclusiveRefundRescindedWhenTxNarrowsDown.  Flag, freeze and
//     refund travel together.
//   - This is not a source of consensus divergence.  `permanent_exclusive_`
//     never participates in an admission decision while a change is pending --
//     `ItemLock` branches on `is_exclusive()` alone -- so it only selects the
//     terminal lock state.  Whether a later transaction is admitted is a
//     deterministic function of the block's transaction sequence, which every
//     validator replays identically, and the incremental `DbHash` still
//     matches the hash recomputed from scratch.
//
// The flag does exactly what it says when it is on the delta that commits:
// each admission test below is paired with a control in which the final write
// keeps the flag and the later transaction is rejected with
// `kDbConflictExclusiveLock`.
//
// See: https://github.com/groundhogchain/hogd/issues/3006

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "contract.hh"
#include "db_conflict.hh"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_cost.hh"
#include "storage_item.hh"
#include "types.hh"

#include "hogd/base/io.h"
#include "hogd/base/perthread_arc.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_db/auth_change_subscription_manager.h"
#include "hogd/hog_db/background_commit_worker.h"
#include "hogd/hog_db/block_tx.h"
#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_db/hog_types.h"
#include "hogd/hog_db/invoke_tx.h"
#include "hogd/hog_db/item_change.h"
#include "hogd/hog_db/item_lock.h"
#include "hogd/hog_db/item_lock/concepts.h"
#include "hogd/hog_db/status.h"
#include "hogd/ramdb/db.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/storage_cost/storage_cost_model.h"

namespace hogd {

namespace {

constexpr auto kNoFlags = std::to_underlying(StorageDeltaFlags::None);
constexpr auto kExclusiveFlags =
    std::to_underlying(StorageDeltaFlags::ExclusiveLock);

constexpr StorageCostModel kTestCostModel =
    StorageCostModel(12000, 1, 1, 1, 1234);

LockContext
make_dummy_context()
{
  static HogDbDAO *dummy_dao = nullptr;
  static ItemKey dummy_key = make_item_key(make_address(0), 0);
  return LockContext(dummy_dao, 0, dummy_key, &kTestCostModel);
}

ItemChange
make_nnint_change(int128_t set, int128_t add)
{
  return NNInt128(set, add);
}

ItemChange
make_rcq_insert_change(uint8_t value)
{
  RetrocausalQueueDelta delta;
  delta.insert.added_entries.push_back(QueueEntry{value});
  return ItemChange(RetrocausalQueueIt(std::move(delta)));
}

RetrocausalQueueDelta
make_rcq_insert_delta(uint8_t value)
{
  RetrocausalQueueDelta delta;
  delta.insert.added_entries.push_back(QueueEntry{value});
  return delta;
}

} // namespace

//
// Layer 1: ItemChange::narrow().
//

// narrow() lifts a later change to ordinary exclusivity so it cannot be
// interleaved with, but the permanent-exclusive request belongs to whichever
// change the transaction ends up committing.
TEST(PermanentExclusiveNarrowWaiTest, NarrowPropagatesExclusivityNotPermanence)
{
  using namespace std::literals;

  // Same item type, compatible values: the earlier change being exclusive is
  // enough to promote the later one (ItemChange::narrow() short-circuits on
  // `change.exclusive_`), but permanence is not inherited.
  {
    auto earlier = make_nnint_change(100, -10);
    earlier.set_permanent_exclusive();
    auto later = make_nnint_change(100, -20);

    ASSERT_TRUE(later.narrow(earlier).has_value());
    EXPECT_TRUE(later.is_exclusive());
    EXPECT_FALSE(later.is_permanent_exclusive());
  }

  // Type transition (the shape reported in #3006: a flagged bytestring write
  // replaced by an unflagged retrocausal-queue write).
  {
    ItemChange earlier(Bytes(std::as_bytes(std::span("frozen"sv))));
    earlier.set_permanent_exclusive();
    auto later = make_rcq_insert_change(0x51);

    ASSERT_TRUE(later.narrow(earlier).has_value());
    EXPECT_TRUE(later.is_exclusive());
    EXPECT_FALSE(later.is_permanent_exclusive());
  }

  // The rule is last-write-wins, not "drop the bit": a later change that
  // carries the flag itself keeps it, whether or not the earlier one had it.
  {
    auto earlier = make_nnint_change(100, -10);
    auto later = make_nnint_change(200, -20);
    later.set_permanent_exclusive();

    ASSERT_TRUE(later.narrow(earlier).has_value());
    EXPECT_TRUE(later.is_exclusive());
    EXPECT_TRUE(later.is_permanent_exclusive());
  }
}

//
// Layer 2: ItemLock.
//

// A permanent-exclusive change reserves a negative marginal cost -- it may
// hand back the item's snapshot cost because nothing else can re-grow the item
// in this block.  Narrowing down to an unflagged change rescinds that refund
// in the same step that it drops the freeze.
TEST(PermanentExclusiveNarrowWaiTest,
     PermanentExclusiveRefundRescindedWhenTxNarrowsDown)
{
  constexpr int128_t kSnapshotExcess = 1000;
  ItemLock lock(kTestCostModel.kBaseItemCost + kSnapshotExcess);

  auto flagged = make_nnint_change(100, 10);
  flagged.set_permanent_exclusive();

  auto r1 =
      lock.try_atomic_change_lock(nullptr, &flagged, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), -kSnapshotExcess)
      << "a permanent-exclusive reservation may reserve the snapshot refund";

  auto narrowed = make_nnint_change(200, 20);
  ASSERT_TRUE(narrowed.narrow(flagged).has_value());
  ASSERT_TRUE(narrowed.is_exclusive());
  ASSERT_FALSE(narrowed.is_permanent_exclusive());

  auto r2 =
      lock.try_atomic_change_lock(&flagged, &narrowed, make_dummy_context());
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value(), 0)
      << "dropping the freeze also gives back the refund it paid for";
}

// The finalized lock state follows the committed change: narrowed down to an
// unflagged change it is ordinary `kFinalized`, and a later same-block change
// is admitted.  The control lock, committing the flagged change unchanged,
// rejects the identical follower.
TEST(PermanentExclusiveNarrowWaiTest, NarrowedDownCommitFinalizesOrdinaryLock)
{
  constexpr int128_t kSnapshotExcess = 1000;

  ItemLock lock(kTestCostModel.kBaseItemCost + kSnapshotExcess);
  auto flagged = make_nnint_change(100, 10);
  flagged.set_permanent_exclusive();
  auto narrowed = make_nnint_change(200, 20);
  ASSERT_TRUE(narrowed.narrow(flagged).has_value());
  ASSERT_FALSE(narrowed.is_permanent_exclusive());

  ASSERT_TRUE(
      lock.try_atomic_change_lock(nullptr, &flagged, make_dummy_context())
          .has_value());
  ASSERT_TRUE(
      lock.try_atomic_change_lock(&flagged, &narrowed, make_dummy_context())
          .has_value());
  EXPECT_EQ(lock.commit_change(narrowed, make_dummy_context()),
            -kSnapshotExcess);

  auto follower = make_nnint_change(200, 1);
  EXPECT_TRUE(
      lock.try_atomic_change_lock(nullptr, &follower, make_dummy_context())
          .has_value())
      << "the committed change did not request a block-long freeze";

  // Control: the same flagged change, committed without being superseded,
  // does freeze the item for the rest of the block.
  ItemLock control(kTestCostModel.kBaseItemCost + kSnapshotExcess);
  auto control_flagged = make_nnint_change(100, 10);
  control_flagged.set_permanent_exclusive();
  ASSERT_TRUE(control
                  .try_atomic_change_lock(nullptr, &control_flagged,
                                          make_dummy_context())
                  .has_value());
  EXPECT_EQ(control.commit_change(control_flagged, make_dummy_context()),
            -kSnapshotExcess);

  auto control_follower = make_nnint_change(100, 1);
  auto rejected = control.try_atomic_change_lock(nullptr, &control_follower,
                                                 make_dummy_context());
  ASSERT_FALSE(rejected.has_value());
}

//
// Layer 3: whole-block behavior through InvokeTX / BlockTX.
//

// NOLINTNEXTLINE(misc-use-internal-linkage)
class PermanentExclusiveNarrowWaiDbTest : public testing::Test {
protected:
  void SetUp() override
  {
    ErrnoStatus<Directory> dir = Directory::open_temporary();
    ASSERT_TRUE(static_cast<bool>(dir));

    constexpr size_t kHeapSize = 10_MiB;
    DBStatus<std::unique_ptr<Database>> db =
        Database::open(dir->get_path(), Database::estimate_heap_size(kHeapSize),
                       0);
    ASSERT_TRUE(db);
    db_ = HogDbDAO(RAMDB(std::move(*db)));
  }

  void setup_contract_address(const Address &addr)
  {
    ASSERT_TRUE(db_.write_address_balance(addr, 10'000'000));
    ASSERT_TRUE(db_.write_address_metadata(
        addr,
        DeployedContractHash{.hash = sha256(std::as_bytes(std::span("seed"))),
                             .contract_size = 10}));
  }

  HogDbDAO db_;

  StorageCostModel cost_model_ = StorageCostModel(StorageCostModelXdr{
      .ext = ExtensionPoint(0),
      .base_data_item_cost = 12000,
      .bytestring_cost_per_byte = 10,
      .base_contract_deployment_cost = 12000,
      .contract_deployment_cost_per_byte = 10,
      .replay_cache_cost_per_entry = 7,
  });

  AuthChangeSubscriptionManager subscription_manager_;
  BackgroundCommitWorker background_commit_worker_ =
      BackgroundCommitWorker(subscription_manager_, nullptr);
};

constexpr uint64_t kWaiAddress = 0xdeadbeef;

// #3006 as filed: a flagged bytestring write superseded by an unflagged RCQ
// write on the same key.  The second transaction is admitted, both entries
// land in the queue, and the incremental block hash still matches the hash
// recomputed from the committed state -- the outcome is a plain function of
// the block's transaction order, not a divergence.
TEST_F(PermanentExclusiveNarrowWaiDbTest,
       BytesExclusiveThenUnflaggedRcqAdmitsLaterTx)
{
  Address addr = make_address(kWaiAddress + 421);
  ItemKey rcq_key = make_item_key(addr, 1);
  setup_contract_address(addr);

  StorageLongHash base_hash = db_.compute_full_db_hash();
  PerThreadARC<BlockTX> block_tx(
      db_, cost_model_, background_commit_worker_, 1,
      BlockExecutionMode::kBlockExecutionModeProposal);

  {
    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx(block_tx);

    constexpr std::array kBytes = {std::byte{0xA5}};
    ASSERT_TRUE(
        invoke_tx.request_bytes(rcq_key, std::span(kBytes), kExclusiveFlags));
    // Supersedes the flagged write, flag included.
    ASSERT_TRUE(invoke_tx.request_retrocausal_queue(rcq_key,
                                                    make_rcq_insert_delta(0x51),
                                                    kNoFlags));

    ASSERT_TRUE(invoke_tx.sync_balance_locks());
    ASSERT_TRUE(invoke_tx.commit());
  }

  {
    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx(block_tx);

    ASSERT_TRUE(invoke_tx.request_retrocausal_queue(rcq_key,
                                                    make_rcq_insert_delta(0x52),
                                                    kNoFlags))
        << "the committed change carried no ExclusiveLock request";

    ASSERT_TRUE(invoke_tx.sync_balance_locks());
    ASSERT_TRUE(invoke_tx.commit());
  }

  auto hash_result = block_tx.get().close(base_hash);
  ASSERT_TRUE(hash_result.has_value());
  StorageLongHash incremental_hash = *hash_result;
  ASSERT_TRUE(block_tx.get().commit());
  EXPECT_EQ(incremental_hash, db_.compute_full_db_hash())
      << "both entries are hashed consistently; nothing diverges";

  auto block_result = db_.read_retrocausal_queue_block(rcq_key, 1);
  ASSERT_TRUE(block_result.has_value());
  ASSERT_EQ(block_result->entries.size(), 2U);
  EXPECT_EQ(block_result->entries[0], QueueEntry{0x51});
  EXPECT_EQ(block_result->entries[1], QueueEntry{0x52});
}

// Control for the test above: keep the flag on the RCQ write that actually
// commits and the later transaction is rejected.
TEST_F(PermanentExclusiveNarrowWaiDbTest,
       BytesExclusiveThenFlaggedRcqBlocksLaterTx)
{
  Address addr = make_address(kWaiAddress + 422);
  ItemKey rcq_key = make_item_key(addr, 1);
  setup_contract_address(addr);

  PerThreadARC<BlockTX> block_tx(
      db_, cost_model_, background_commit_worker_, 1,
      BlockExecutionMode::kBlockExecutionModeProposal);

  {
    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx(block_tx);

    constexpr std::array kBytes = {std::byte{0xA5}};
    ASSERT_TRUE(
        invoke_tx.request_bytes(rcq_key, std::span(kBytes), kExclusiveFlags));
    ASSERT_TRUE(invoke_tx.request_retrocausal_queue(rcq_key,
                                                    make_rcq_insert_delta(0x51),
                                                    kExclusiveFlags));

    ASSERT_TRUE(invoke_tx.sync_balance_locks());
    ASSERT_TRUE(invoke_tx.commit());
  }

  {
    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx(block_tx);

    auto admitted =
        invoke_tx.request_retrocausal_queue(rcq_key,
                                            make_rcq_insert_delta(0x52),
                                            kNoFlags);
    ASSERT_FALSE(admitted.has_value());
    EXPECT_EQ(invoke_tx.conflict_reason_for(admitted.error().code()),
              DbConflictCode::kDbConflictExclusiveLock);
  }

  ASSERT_TRUE(block_tx.get().close(StorageLongHash{}).has_value());
  ASSERT_TRUE(block_tx.get().commit());

  auto block_result = db_.read_retrocausal_queue_block(rcq_key, 1);
  ASSERT_TRUE(block_result.has_value());
  EXPECT_EQ(block_result->entries.size(), 1U);
}

// The NNInt shape of the same thing (amendments on #3006): a flagged NNInt
// reservation superseded by an unflagged NNInt write in the same InvokeTX.
TEST_F(PermanentExclusiveNarrowWaiDbTest,
       NNIntExclusiveThenUnflaggedWriteAdmitsLaterTx)
{
  Address addr = make_address(kWaiAddress + 423);
  ItemKey data_key = make_item_key(addr, 1);
  setup_contract_address(addr);

  PerThreadARC<BlockTX> block_tx(
      db_, cost_model_, background_commit_worker_, 1,
      BlockExecutionMode::kBlockExecutionModeProposal);

  {
    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx(block_tx);

    ASSERT_TRUE(invoke_tx.request_nnint128(data_key, /*base=*/0, /*delta=*/100,
                                           kExclusiveFlags));
    ASSERT_TRUE(invoke_tx.request_nnint128(data_key, /*base=*/0, /*delta=*/200,
                                           kNoFlags));

    ASSERT_TRUE(invoke_tx.sync_balance_locks());
    ASSERT_TRUE(invoke_tx.commit());
  }

  {
    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx(block_tx);

    ASSERT_TRUE(invoke_tx.request_nnint128(data_key, /*base=*/0, /*delta=*/1,
                                           kNoFlags));

    ASSERT_TRUE(invoke_tx.sync_balance_locks());
    ASSERT_TRUE(invoke_tx.commit());
  }

  ASSERT_TRUE(block_tx.get().close(StorageLongHash{}).has_value());
  ASSERT_TRUE(block_tx.get().commit());

  auto committed = db_.read_nnint128(data_key);
  ASSERT_TRUE(committed.has_value());
  EXPECT_EQ(*committed, 201);
}

// Control: the flag stays on the NNInt write that commits.
TEST_F(PermanentExclusiveNarrowWaiDbTest,
       NNIntExclusiveOnCommittedWriteBlocksLaterTx)
{
  Address addr = make_address(kWaiAddress + 424);
  ItemKey data_key = make_item_key(addr, 1);
  setup_contract_address(addr);

  PerThreadARC<BlockTX> block_tx(
      db_, cost_model_, background_commit_worker_, 1,
      BlockExecutionMode::kBlockExecutionModeProposal);

  {
    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx(block_tx);

    ASSERT_TRUE(invoke_tx.request_nnint128(data_key, /*base=*/0, /*delta=*/100,
                                           kExclusiveFlags));
    ASSERT_TRUE(invoke_tx.request_nnint128(data_key, /*base=*/0, /*delta=*/200,
                                           kExclusiveFlags));

    ASSERT_TRUE(invoke_tx.sync_balance_locks());
    ASSERT_TRUE(invoke_tx.commit());
  }

  {
    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx(block_tx);

    auto admitted = invoke_tx.request_nnint128(data_key, /*base=*/0,
                                               /*delta=*/1, kNoFlags);
    ASSERT_FALSE(admitted.has_value());
    EXPECT_EQ(invoke_tx.conflict_reason_for(admitted.error().code()),
              DbConflictCode::kDbConflictExclusiveLock);
  }

  ASSERT_TRUE(block_tx.get().close(StorageLongHash{}).has_value());
  ASSERT_TRUE(block_tx.get().commit());

  auto committed = db_.read_nnint128(data_key);
  ASSERT_TRUE(committed.has_value());
  EXPECT_EQ(*committed, 200);
}

// A flagged deletion superseded by an unflagged NNInt write behaves the same
// way; the type transition is not what matters, the committed flag is.
TEST_F(PermanentExclusiveNarrowWaiDbTest,
       DeleteExclusiveThenUnflaggedNNIntAdmitsLaterTx)
{
  Address addr = make_address(kWaiAddress + 425);
  ItemKey data_key = make_item_key(addr, 1);
  setup_contract_address(addr);

  PerThreadARC<BlockTX> block_tx(
      db_, cost_model_, background_commit_worker_, 1,
      BlockExecutionMode::kBlockExecutionModeProposal);

  {
    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx(block_tx);

    ASSERT_TRUE(invoke_tx.request_deletion(data_key, kExclusiveFlags));
    ASSERT_TRUE(invoke_tx.request_nnint128(data_key, /*base=*/0, /*delta=*/10,
                                           kNoFlags));

    ASSERT_TRUE(invoke_tx.sync_balance_locks());
    ASSERT_TRUE(invoke_tx.commit());
  }

  {
    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx(block_tx);

    ASSERT_TRUE(invoke_tx.request_nnint128(data_key, /*base=*/0, /*delta=*/5,
                                           kNoFlags));

    ASSERT_TRUE(invoke_tx.sync_balance_locks());
    ASSERT_TRUE(invoke_tx.commit());
  }

  ASSERT_TRUE(block_tx.get().close(StorageLongHash{}).has_value());
  ASSERT_TRUE(block_tx.get().commit());

  auto committed = db_.read_nnint128(data_key);
  ASSERT_TRUE(committed.has_value());
  EXPECT_EQ(*committed, 15);
}

// The invariant narrow() exists to protect (item_change.h): a rollback can
// never fail on a lock some other transaction took in the meantime.  Ordinary
// exclusivity is what keeps others out while the change is pending, so the
// pre-savepoint permanent-exclusive version is still there to restore -- and
// restoring it restores the freeze too.
TEST_F(PermanentExclusiveNarrowWaiDbTest,
       RollbackToSavepointRestoresPermanentExclusiveReservation)
{
  Address addr = make_address(kWaiAddress + 426);
  ItemKey data_key = make_item_key(addr, 1);
  setup_contract_address(addr);

  PerThreadARC<BlockTX> block_tx(
      db_, cost_model_, background_commit_worker_, 1,
      BlockExecutionMode::kBlockExecutionModeProposal);

  {
    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx(block_tx);

    // Each subtransaction is a savepoint that ends in sync_balance_locks(),
    // mirroring HogDbExecutorRpc::try_push_new_subtransaction().
    invoke_tx.create_savepoint();
    ASSERT_TRUE(invoke_tx.request_nnint128(data_key, /*base=*/0, /*delta=*/100,
                                           kExclusiveFlags));
    ASSERT_TRUE(invoke_tx.sync_balance_locks());

    int second = invoke_tx.create_savepoint();
    ASSERT_TRUE(invoke_tx.request_nnint128(data_key, /*base=*/0, /*delta=*/200,
                                           kNoFlags));
    ASSERT_TRUE(invoke_tx.sync_balance_locks());
    invoke_tx.rollback_to_savepoint(second - 1);

    ASSERT_TRUE(invoke_tx.sync_balance_locks());
    ASSERT_TRUE(invoke_tx.commit());
  }

  {
    ASSERT_TRUE(block_tx.try_acquire_reference());
    InvokeTX invoke_tx(block_tx);

    auto admitted = invoke_tx.request_nnint128(data_key, /*base=*/0,
                                               /*delta=*/1, kNoFlags);
    ASSERT_FALSE(admitted.has_value())
        << "the restored version is the flagged one, so the freeze applies";
    EXPECT_EQ(invoke_tx.conflict_reason_for(admitted.error().code()),
              DbConflictCode::kDbConflictExclusiveLock);
  }

  ASSERT_TRUE(block_tx.get().close(StorageLongHash{}).has_value());
  ASSERT_TRUE(block_tx.get().commit());

  auto committed = db_.read_nnint128(data_key);
  ASSERT_TRUE(committed.has_value());
  EXPECT_EQ(*committed, 100);
}

} // namespace hogd
