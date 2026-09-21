#include <gtest/gtest.h>

#include <limits>
#include <optional>

#include "db_conflict.hh"
#include "hogd/base/types.h"
#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_db/hog_types.h"
#include "hogd/hog_db/item_change.h"
#include "hogd/hog_db/item_lock/balance_lock.h"
#include "hogd/hog_db/item_lock/concepts.h"
#include "hogd/hog_db/status.h"
#include "hogd/storage_cost/storage_cost_model.h"

namespace hogd {

namespace {
constexpr StorageCostModel kTestCostModel =
    StorageCostModel(12000, 1, 1, 1, 1234);

// Helper to create a dummy LockContext for testing
inline LockContext
make_dummy_context()
{
  // These are never dereferenced in most lock implementations
  static HogDbDAO *dummy_dao = nullptr;
  static ItemKey dummy_key = make_address_balance_item_key(make_address(0));
  return LockContext(dummy_dao, 0, dummy_key, &kTestCostModel);
}

} // namespace

TEST(BalanceLockTest, BasicUsage)
{
  BalanceLock lock(100);

  Balance after(10);
  auto status =
      lock.try_atomic_change_lock(nullptr, &after, make_dummy_context());
  EXPECT_TRUE(status.has_value());
  EXPECT_EQ(status.value(), 0);
}

TEST(BalanceLockTest, AddOverflow)
{

  BalanceLock lock(1);
  Balance after(std::numeric_limits<int128_t>::max());
  auto status =
      lock.try_atomic_change_lock(nullptr, &after, make_dummy_context());
  EXPECT_FALSE(status.has_value());

  Balance good_after(std::numeric_limits<int128_t>::max() - 1);
  status =
      lock.try_atomic_change_lock(nullptr, &good_after, make_dummy_context());
  EXPECT_TRUE(status.has_value());
  EXPECT_EQ(status.value(), 0);
}

TEST(BalanceLockTest, SubOverflow)
{
  BalanceLock lock(10);
  Balance after(-10);
  auto status =
      lock.try_atomic_change_lock(nullptr, &after, make_dummy_context());
  EXPECT_TRUE(status.has_value());

  Balance another_sub(-5);
  status =
      lock.try_atomic_change_lock(nullptr, &another_sub, make_dummy_context());
  EXPECT_FALSE(status.has_value());

  // release the held lock
  EXPECT_TRUE(lock.try_atomic_change_lock(&after, nullptr, make_dummy_context())
                  .has_value());

  // now the lock should be available
  status =
      lock.try_atomic_change_lock(nullptr, &another_sub, make_dummy_context());
  EXPECT_TRUE(status.has_value());
}

TEST(BalanceLockTest, AddAndSubNarrowing)
{
  BalanceLock lock(100);

  // once we acquire this lock, no further adds should be possible
  Balance first(std::numeric_limits<int128_t>::max() - 100);

  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &first, make_dummy_context())
                  .has_value());

  Balance second(-100);
  EXPECT_TRUE(second.narrow(first).has_value());
  EXPECT_TRUE(lock.try_atomic_change_lock(&first, &second, make_dummy_context())
                  .has_value());

  // we already have the lock for this one
  Balance third(100);
  EXPECT_TRUE(third.narrow(second).has_value());
  EXPECT_TRUE(lock.try_atomic_change_lock(&second, &third, make_dummy_context())
                  .has_value());

  // we don't have the lock for more subtractions
  Balance fourth(-101);
  EXPECT_FALSE(
      lock.try_atomic_change_lock(&third, &fourth, make_dummy_context())
          .has_value());

  // Concurrent txs can't add or sub
  Balance othertx1_first(100);

  EXPECT_FALSE(lock.try_atomic_change_lock(nullptr, &othertx1_first,
                                           make_dummy_context())
                   .has_value());

  Balance othertx2_first(-100);
  EXPECT_FALSE(lock.try_atomic_change_lock(nullptr, &othertx2_first,
                                           make_dummy_context())
                   .has_value());

  // If we release the lock, the others can succeed
  EXPECT_TRUE(lock.try_atomic_change_lock(&third, nullptr, make_dummy_context())
                  .has_value());

  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &othertx1_first,
                                          make_dummy_context())
                  .has_value());
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &othertx2_first,
                                          make_dummy_context())
                  .has_value());

  EXPECT_EQ(lock.get_add_capacity(),
            std::numeric_limits<int128_t>::max() - 200);
  EXPECT_EQ(lock.get_sub_capacity(), 0);
}

TEST(BalanceLockTest, AddAndSubAcquireAndRelease)
{

  BalanceLock lock(100);

  const Balance add(100);
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &add, make_dummy_context())
                  .has_value());

  Balance sub(-100);
  ASSERT_TRUE(sub.narrow(add).value());
  EXPECT_TRUE(lock.try_atomic_change_lock(&add, &sub, make_dummy_context())
                  .has_value());

  EXPECT_TRUE(lock.try_atomic_change_lock(&sub, nullptr, make_dummy_context())
                  .has_value());

  EXPECT_EQ(lock.get_add_capacity(),
            std::numeric_limits<int128_t>::max() - 100);
  EXPECT_EQ(lock.get_sub_capacity(), 100);
}

TEST(BalanceLockTest, HalfAcquireLock)
{

  BalanceLock lock(100);

  Balance too_much_add(std::numeric_limits<int128_t>::max() - 99 - 50);

  // workaround: Balance is considered invalid if base + delta < 0, or if base
  // + delta overflows
  Balance ok_sub(-50);
  EXPECT_TRUE(
      lock.try_atomic_change_lock(nullptr, &ok_sub, make_dummy_context())
          .has_value());
  Balance ok_add(50);
  EXPECT_TRUE(
      lock.try_atomic_change_lock(nullptr, &ok_add, make_dummy_context())
          .has_value());

  Balance too_much_sub(-51);

  EXPECT_FALSE(
      lock.try_atomic_change_lock(nullptr, &too_much_add, make_dummy_context())
          .has_value());
  EXPECT_FALSE(
      lock.try_atomic_change_lock(nullptr, &too_much_sub, make_dummy_context())
          .has_value());

  Balance good_sub(-50);
  EXPECT_TRUE(good_sub.narrow(too_much_add).has_value());
  EXPECT_FALSE(
      lock.try_atomic_change_lock(nullptr, &good_sub, make_dummy_context())
          .has_value());

  Balance good_add(100);
  EXPECT_TRUE(good_add.narrow(too_much_sub).has_value());
  EXPECT_FALSE(
      lock.try_atomic_change_lock(nullptr, &good_add, make_dummy_context())
          .has_value());

  EXPECT_EQ(lock.get_add_capacity(),
            std::numeric_limits<int128_t>::max() - 100 - 50);
  EXPECT_EQ(lock.get_sub_capacity(), 50);
}

TEST(BalanceLockTest, MultipleAddsOnValue)
{
  BalanceLock lock(100);

  Balance add(100);
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &add, make_dummy_context())
                  .has_value());

  Balance add2(50);
  EXPECT_TRUE(add2.narrow(add).has_value());
  EXPECT_TRUE(lock.try_atomic_change_lock(&add, &add2, make_dummy_context())
                  .has_value());
  EXPECT_EQ(lock.get_add_capacity(),
            std::numeric_limits<int128_t>::max() - 200);
  EXPECT_EQ(lock.get_sub_capacity(), 100);

  EXPECT_EQ(lock.commit_change(true, 0, add2, make_dummy_context()), 0);
}

TEST(BalanceLockTest, NewBalanceStorageCost)
{
  // When creating a new balance (snapshot_value is nullopt), the delta must
  // cover the storage cost
  BalanceLock lock(std::nullopt);

  constexpr static int128_t kBalanceStorageCost =
      kTestCostModel.get_balance_cost();
  ASSERT_GT(kBalanceStorageCost, 0);

  // Storage cost is 1234, so a delta less than that should fail
  Balance too_small(kBalanceStorageCost - 1);
  auto status =
      lock.try_atomic_change_lock(nullptr, &too_small, make_dummy_context());
  EXPECT_FALSE(status.has_value());

  // A delta equal to or greater than storage cost should succeed
  Balance sufficient(kBalanceStorageCost);
  status =
      lock.try_atomic_change_lock(nullptr, &sufficient, make_dummy_context());
  EXPECT_TRUE(status.has_value());

  EXPECT_EQ(lock.build_result(make_dummy_context()), 0);

  // Adding additional is ok
  Balance larger(10000);
  status = lock.try_atomic_change_lock(nullptr, &larger, make_dummy_context());
  EXPECT_TRUE(status.has_value());

  // transferred kBalanceStorageCost + 10000 to the address, and only pay once
  EXPECT_EQ(lock.build_result(make_dummy_context()), 10000);
}

// Regression test for #2957: modifying an existing balance reservation into an
// opposite-sign extreme delta makes delta_after - delta_before overflow inside
// NNInt128DeltaBounds::try_reserve_delta. Unlike NNInt128 deltas, balance
// deltas are not bounded against a base, so a contract can reach this via a
// narrowed reservation (e.g. -1 then INT128_MAX, or +1 then INT128_MIN). This
// must surface as a recoverable bounds conflict, not abort the process.
TEST(BalanceLockTest, OppositeSignDeltaOverflowReturnsConflict)
{
  static ItemKey balance_key =
      make_address_balance_item_key(make_address(2957));

  auto make_ctx = [](std::optional<DbConflictCode> *reason) {
    static HogDbDAO *dummy_dao = nullptr;
    LockContext ctx(dummy_dao, 0, balance_key, &kTestCostModel);
    ctx.conflict_reason = reason;
    return ctx;
  };

  // Negative-then-positive: delta_before = -1, delta_after = INT128_MAX.
  {
    BalanceLock lock(1);

    Balance first(-1);
    ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &first, make_ctx(nullptr))
                    .has_value());

    Balance second(std::numeric_limits<int128_t>::max());
    ASSERT_TRUE(second.narrow(first).has_value());

    std::optional<DbConflictCode> reason;
    auto status =
        lock.try_atomic_change_lock(&first, &second, make_ctx(&reason));
    ASSERT_FALSE(status.has_value());
    EXPECT_EQ(status.error().code(), HogDbCode::kConflict);
    EXPECT_EQ(reason, DbConflictCode::kDbConflictBoundsExceeded);
  }

  // Positive-then-negative: delta_before = +1, delta_after = INT128_MIN.
  {
    BalanceLock lock(100);

    Balance first(1);
    ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &first, make_ctx(nullptr))
                    .has_value());

    Balance second(std::numeric_limits<int128_t>::min());
    ASSERT_TRUE(second.narrow(first).has_value());

    std::optional<DbConflictCode> reason;
    auto status =
        lock.try_atomic_change_lock(&first, &second, make_ctx(&reason));
    ASSERT_FALSE(status.has_value());
    EXPECT_EQ(status.error().code(), HogDbCode::kConflict);
    EXPECT_EQ(reason, DbConflictCode::kDbConflictBoundsExceeded);
  }
}

// A fresh balance (constructed from std::nullopt) reserves aggregate credit
// capacity against a logical base of 0, so it can absorb at most INT128_MAX
// before overflowing. This is the contract ItemLock relies on when it decodes a
// missing snapshot balance; see ItemLockTest.MissingBalanceSnapshotIsFresh for
// the boundary regression test for #3809.
TEST(BalanceLockTest, FreshBalanceCapacityIsInt128Max)
{
  constexpr int128_t kMax = std::numeric_limits<int128_t>::max();

  BalanceLock lock(std::nullopt);
  EXPECT_EQ(lock.get_add_capacity(), kMax);

  // A first credit can fill the fresh balance to its true capacity...
  Balance first(kMax);
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &first, make_dummy_context())
                  .has_value());
  // ...after which no further credit fits, so the aggregate delta cannot exceed
  // INT128_MAX and build_result cannot overflow.
  Balance second(kTestCostModel.get_balance_cost());
  EXPECT_FALSE(
      lock.try_atomic_change_lock(nullptr, &second, make_dummy_context())
          .has_value());
}

// --- Storage-refund path (BalanceLock::add_committed_credit) -----------------
//
// A tx reserves the full storage cost of every row it writes, but only the
// commit that actually materializes a row keeps that cost; a duplicate
// same-value write gets its reservation back through
// BlockTX::release_balance -> ItemLock::add_committed_balance_credit ->
// BalanceLock::add_committed_credit, on a lock that is already finalized.
//
// #4096 changed how that refund is applied: it used to reserve *fresh positive
// add-capacity* and CHECK the reservation succeeded, which aborted the process
// whenever the remaining add-capacity was smaller than the refund (#4095). It
// now bumps the committed delta directly and leaves the add_/sub_ reservation
// counters alone, so the tests below pin both halves of that contract -- the
// refund lands in the committed value, and it costs no capacity.

// The refund moves the committed delta and nothing else. Reservation counters
// bound *concurrent* locking; a refund is not a reservation, so neither the
// add nor the sub counter may move.
TEST(BalanceLockTest, RefundDoesNotConsumeCapacity)
{
  constexpr int128_t kMax = std::numeric_limits<int128_t>::max();

  BalanceLock lock(100);
  ASSERT_EQ(lock.get_add_capacity(), kMax - 100);
  ASSERT_EQ(lock.get_sub_capacity(), 100);

  // A tx locks storage out of the balance: a debit draws on sub-capacity only.
  const Balance debit(-50);
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &debit, make_dummy_context())
                  .has_value());
  ASSERT_EQ(lock.get_delta(), -50);
  ASSERT_EQ(lock.get_add_capacity(), kMax - 100);
  ASSERT_EQ(lock.get_sub_capacity(), 50);

  // Its row turns out to be already materialized, so the reservation comes
  // back after the lock is finalized.
  lock.add_committed_credit(50);

  EXPECT_EQ(lock.get_delta(), 0);
  EXPECT_EQ(lock.build_result(make_dummy_context()), 100);
  EXPECT_EQ(lock.get_add_capacity(), kMax - 100)
      << "a refund must not consume add-capacity";
  EXPECT_EQ(lock.get_sub_capacity(), 50)
      << "a refund must not hand back sub-capacity either";
}

// Regression test for #4095, at the lock level: when remaining add-capacity is
// smaller than a single refund, the refund must still be applied. The old
// implementation reserved add-capacity for the refund and CHECK-failed here,
// aborting the validator. See
// HogDbTest.DuplicateBytestringRefundNearMaxBalanceCommits for the same case
// driven end to end through two duplicate writers.
TEST(BalanceLockTest, RefundCommitsWhenAddCapacityIsSmallerThanRefund)
{
  constexpr int128_t kMax = std::numeric_limits<int128_t>::max();
  constexpr int128_t kRowCost = kTestCostModel.get_nnint_cost();

  // One unit short of a single refund -- the exact condition that used to trip
  // CHECK(ok) inside add_committed_credit.
  constexpr int128_t kBase = kMax - (kRowCost - 1);
  BalanceLock lock(kBase);
  ASSERT_LT(lock.get_add_capacity(), kRowCost);

  const Balance debit(-kRowCost);
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &debit, make_dummy_context())
                  .has_value());

  lock.add_committed_credit(kRowCost);

  EXPECT_EQ(lock.get_delta(), 0);
  EXPECT_EQ(lock.build_result(make_dummy_context()), kBase);
  EXPECT_EQ(lock.get_add_capacity(), kRowCost - 1)
      << "the refund must not have drawn on the capacity it cannot afford";
}

// A balance sitting at exactly INT128_MAX has *zero* add-capacity, so the old
// implementation aborted on any refund at all. The refund path no longer
// consults add-capacity, and returning the reservation puts the committed value
// back at exactly INT128_MAX -- one unit from tripping build_result's overflow
// CHECK. (HOG conservation makes a max-balance address holding locked storage
// unreachable in production -- see
// NativeTokenAllowanceTest.StorageRefundCreditsAgainstNearMaxBalance -- but
// add_committed_credit is infallible and must be total at the boundary.)
TEST(BalanceLockTest, RefundAgainstZeroAddCapacityCommits)
{
  constexpr int128_t kMax = std::numeric_limits<int128_t>::max();
  constexpr int128_t kRowCost = kTestCostModel.get_nnint_cost();

  BalanceLock lock(kMax);
  ASSERT_EQ(lock.get_add_capacity(), 0);

  const Balance debit(-kRowCost);
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &debit, make_dummy_context())
                  .has_value());
  ASSERT_EQ(lock.build_result(make_dummy_context()), kMax - kRowCost);

  lock.add_committed_credit(kRowCost);

  EXPECT_EQ(lock.get_delta(), 0);
  EXPECT_EQ(lock.build_result(make_dummy_context()), kMax);
  EXPECT_EQ(lock.get_add_capacity(), 0);
}

// The point of applying the refund outside the reservation counters: the
// capacity it would have consumed stays available to the rest of the block. A
// later tx re-enters the finalized lock through try_atomic_change_lock, which
// *does* consult add_, and must still find the full original headroom. Before
// #4096 the refund had eaten kRowCost of it and the credit below would have
// been rejected with kDbConflictBoundsExceeded.
TEST(BalanceLockTest, RefundedCapacityStaysAvailableToALaterReservation)
{
  constexpr int128_t kMax = std::numeric_limits<int128_t>::max();
  constexpr int128_t kRowCost = kTestCostModel.get_nnint_cost();

  constexpr int128_t kBase = kMax - (3 * kRowCost);
  BalanceLock lock(kBase);
  ASSERT_EQ(lock.get_add_capacity(), 3 * kRowCost);

  const Balance debit(-kRowCost);
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &debit, make_dummy_context())
                  .has_value());
  lock.add_committed_credit(kRowCost);
  ASSERT_EQ(lock.get_delta(), 0);

  // A later tx in the same block credits the whole remaining headroom.
  const Balance credit(3 * kRowCost);
  std::optional<DbConflictCode> reason;
  LockContext ctx = make_dummy_context();
  ctx.conflict_reason = &reason;
  EXPECT_TRUE(lock.try_atomic_change_lock(nullptr, &credit, ctx).has_value())
      << "the refund stranded add-capacity the rest of the block needs";
  EXPECT_FALSE(reason.has_value());

  EXPECT_EQ(lock.get_add_capacity(), 0);
  EXPECT_EQ(lock.build_result(make_dummy_context()), kMax);
}

// Several duplicate writers of the same row each reserve its cost, and every
// commit after the first refunds one. The refunds accumulate in the committed
// delta, the net charge is exactly the one materialized row, and none of them
// touches the capacity counters.
TEST(BalanceLockTest, MultipleRefundsAccumulate)
{
  constexpr int128_t kMax = std::numeric_limits<int128_t>::max();
  constexpr int128_t kRowCost = kTestCostModel.get_nnint_cost();

  constexpr int128_t kBase = kMax - (kRowCost - 1);
  BalanceLock lock(kBase);

  // Three concurrent txs each reserve the row's full cost.
  const Balance debit(-kRowCost);
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(
        lock.try_atomic_change_lock(nullptr, &debit, make_dummy_context())
            .has_value())
        << "reservation " << i << " failed";
  }
  ASSERT_EQ(lock.get_delta(), -3 * kRowCost);

  // Only the first commit materializes the row; the other two refund.
  lock.add_committed_credit(kRowCost);
  lock.add_committed_credit(kRowCost);

  EXPECT_EQ(lock.get_delta(), -kRowCost);
  EXPECT_EQ(lock.build_result(make_dummy_context()), kBase - kRowCost);
  // What the mempool scheduler observes for this address.
  EXPECT_EQ(lock.get_committed_delta(make_dummy_context()), -kRowCost);
  EXPECT_EQ(lock.get_add_capacity(), kRowCost - 1);
}

// A refund on a balance that this block created (no snapshot value) still has
// the one-time balance-row cost deducted from build_result, exactly once. A
// fresh balance has zero sub-capacity, so its storage charge can only ride
// along inside the funding credit -- the shape
// HogDbTest.ReleaseBalanceRefundOnFreshAddress{CrossTx,SameTx} exercise end to
// end, here with the refund landing on the finalized lock.
TEST(BalanceLockTest, RefundOnFreshBalanceChargesRowCostOnce)
{
  constexpr int128_t kMax = std::numeric_limits<int128_t>::max();
  constexpr int128_t kRowCost = kTestCostModel.get_nnint_cost();
  constexpr int128_t kBalanceCost = kTestCostModel.get_balance_cost();

  BalanceLock lock(std::nullopt);
  ASSERT_EQ(lock.get_sub_capacity(), 0);

  // Funded, minus the cost of the two rows the same tx locks.
  constexpr int128_t kNetCredit =
      kBalanceCost + (4 * kRowCost) - (2 * kRowCost);
  const Balance funding(kNetCredit);
  ASSERT_TRUE(
      lock.try_atomic_change_lock(nullptr, &funding, make_dummy_context())
          .has_value());

  // One of the two rows was already materialized by a sibling tx.
  lock.add_committed_credit(kRowCost);

  constexpr int128_t kExpected = kNetCredit + kRowCost - kBalanceCost;
  EXPECT_EQ(lock.build_result(make_dummy_context()), kExpected);
  EXPECT_EQ(lock.get_committed_delta(make_dummy_context()), kExpected);
  EXPECT_EQ(lock.get_add_capacity(), kMax - kNetCredit);
}

} // namespace hogd
