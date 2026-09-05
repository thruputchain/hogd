// Tolerated concurrency anomaly: replay cache pending_new_heights_ leak.
//
// When a fresh replay-cache height reservation is created and then released
// (e.g. transaction abort), the lock-wide pending_new_heights_ counter is
// never decremented. This can cause a subsequent valid fresh-height reservation
// to be rejected with kConflict even though the released height will not be
// serialized.
//
// Why this is tolerated:
//   - kConflict propagates as RPCError::kRPCStorageWriteFailed, which is a
//     soft rejection: the transaction is excluded from the current block
//     proposal and retried in the next block
//     (ExecResultTxNonDeterministicError).
//   - It never becomes a ValidationFailure (consensus-critical divergence).
//     The proposer's leaked counter is strictly more restrictive than the
//     validator's (which replays only successful txs and has no leak), so any
//     transaction the proposer includes will also pass validation.
//   - The leak is per-block: each new block creates a fresh ReplayCacheLock
//     with pending_new_heights_ == 0.
//   - Fixing this correctly under concurrent access requires either a
//     per-height lock or accepting a re-add undercount gap. Both add
//     complexity for a benign anomaly.
//
// See: https://github.com/groundhogchain/hogd/issues/2356

#include <gtest/gtest.h>

#include <cstdint>
#include <utility>

#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

// NOLINTBEGIN(bugprone-unchecked-optional-access)

// NOLINTNEXTLINE(misc-use-internal-linkage)
class ReplayCacheHeightBudgetLeakTest : public ::testing::Test,
                                        public NativeContractTestFramework {
public:
  ReplayCacheHeightBudgetLeakTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb)
  {}

  auto make_new_per_tx_db_proxy()
  {
    return NativeContractTestFramework::make_new_per_tx_db_proxy(UINT64_MAX);
  }

  static auto make_nonce_delta(Uniquifier nonce)
  {
    StorageDelta delta(StorageDeltaType::kDeltaTypeConsumeUniquifier);
    delta.consume_uniquifier_args().uniquifier = nonce;
    return delta;
  }
};

// An address with kReplayCacheHeightsMaxLen - 2 persisted heights has room for
// exactly 2 more fresh heights. We reserve one and keep it, reserve a second
// on a different height and abort it, then try a third distinct fresh height.
// The third should fit (1 kept + kPersistedHeights = cap - 1), but the aborted
// reservation still consumes the pending_new_heights_ budget, so the push
// fails.
//
// The test then advances a block and shows that the anomaly is transient: the
// same operation succeeds in the next block.
TEST_F(ReplayCacheHeightBudgetLeakTest,
       AbortedFreshHeightLeaksPendingBudgetButIsTransient)
{
  auto contract_addr = make_contract_address(99);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto &dao = get_hog_db_dao();
  constexpr BlockHeight kSeedBase = 1000;
  constexpr std::int64_t kPersistedHeights = kReplayCacheHeightsMaxLen - 2;

  ReplayCacheMetadata meta;
  meta.size = kPersistedHeights;
  meta.heights.reserve(kPersistedHeights);
  for (std::int64_t i = 0; i < kPersistedHeights; ++i) {
    BlockHeight h = kSeedBase + static_cast<BlockHeight>(i);
    meta.heights.push_back(
        ReplayCacheHeightCount{.height = h, .nonce_count = 1});

    ReplayCacheData data;
    data.nonces.push_back(h);
    ASSERT_TRUE(dao.write_address_replay_cache_data(contract_addr, h, data));
  }
  ASSERT_TRUE(dao.write_address_replay_cache_meta(contract_addr, meta));

  const BlockHeight kept_height =
      kSeedBase + static_cast<BlockHeight>(kReplayCacheHeightsMaxLen) + 1000;
  const BlockHeight dropped_height = kept_height + 1;
  const BlockHeight final_height = kept_height + 2;

  // TX1: reserve a fresh height and commit.
  {
    auto tx = make_new_per_tx_db_proxy();
    tx.add_delta(make_replay_cache_addrkey(contract_addr),
                 make_nonce_delta(Uniquifier{.block_expiry_height = kept_height,
                                             .nonce = 1}),
                 std::to_underlying(StorageDeltaFlags::None));
    auto r = tx.try_push_current_subtransaction()();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
    ASSERT_TRUE(tx.commit_transaction()().has_value());
  }

  // TX2: reserve a different fresh height, then abort.
  {
    auto tx = make_new_per_tx_db_proxy();
    tx.add_delta(make_replay_cache_addrkey(contract_addr),
                 make_nonce_delta(
                     Uniquifier{.block_expiry_height = dropped_height,
                                .nonce = 2}),
                 std::to_underlying(StorageDeltaFlags::None));
    auto r = tx.try_push_current_subtransaction()();
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r.value());
    ASSERT_TRUE(tx.abort_transaction()().has_value());
  }

  // TX3: try a third distinct fresh height. Should fit (cap - 2 persisted +
  // 1 kept = cap - 1, one slot remains), but the leaked counter from the
  // aborted TX2 makes the lock think 2 fresh heights are active.
  {
    auto tx = make_new_per_tx_db_proxy();
    tx.add_delta(make_replay_cache_addrkey(contract_addr),
                 make_nonce_delta(
                     Uniquifier{.block_expiry_height = final_height,
                                .nonce = 3}),
                 std::to_underlying(StorageDeltaFlags::None));
    auto r = tx.try_push_current_subtransaction()();
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r.value())
        << "known anomaly: aborted fresh-height reservation leaks the "
           "pending_new_heights_ budget, causing a false rejection "
           "(issue #2356)";
    ASSERT_TRUE(tx.abort_transaction()().has_value());
  }

  // Advance to a new block: the leak is per-block, so the same operation
  // should now succeed.
  advance_block();

  {
    auto tx = make_new_per_tx_db_proxy();
    tx.add_delta(make_replay_cache_addrkey(contract_addr),
                 make_nonce_delta(
                     Uniquifier{.block_expiry_height = final_height,
                                .nonce = 3}),
                 std::to_underlying(StorageDeltaFlags::None));
    auto r = tx.try_push_current_subtransaction()();
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r.value())
        << "the leak is per-block; a fresh lock in the next block must admit "
           "the same reservation";
    ASSERT_TRUE(tx.commit_transaction()().has_value());
  }

  advance_block();
}

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
