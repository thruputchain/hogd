#include <gtest/gtest.h>

#include <algorithm>

#include "db_conflict.hh"
#include "hogd/base/types.h"
#include "hogd/hog_indexer/recent_tx_failure_cache.h"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

namespace {

SignedTransactionHash
make_hash(uint8_t fill)
{
  SignedTransactionHash hash;
  std::ranges::fill(hash, fill);
  return hash;
}

TxFailureDiagnostic
make_diag(BlockHeight attempt_height, DbConflictCode code)
{
  TxFailureDiagnostic diag;
  diag.attempt_height = attempt_height;
  diag.conflict.code = code;
  return diag;
}

} // namespace

TEST(RecentTxFailureCacheTest, ReportThenLookup)
{
  RecentTxFailureCache cache;
  const auto hash = make_hash(0x01);
  cache.report(hash, make_diag(100, DbConflictCode::kDbConflictValueMismatch),
               100);

  auto found = cache.lookup(hash);
  ASSERT_TRUE(found.has_value());
  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  EXPECT_EQ(found->attempt_height, 100);
  EXPECT_EQ(found->conflict.code, DbConflictCode::kDbConflictValueMismatch);
  // NOLINTEND(bugprone-unchecked-optional-access)
  EXPECT_FALSE(cache.lookup(make_hash(0x02)).has_value());
}

TEST(RecentTxFailureCacheTest, LatestAttemptWins)
{
  RecentTxFailureCache cache;
  const auto hash = make_hash(0x01);
  cache.report(hash, make_diag(10, DbConflictCode::kDbConflictValueMismatch),
               10);
  cache.report(hash, make_diag(12, DbConflictCode::kDbConflictOverflow), 12);
  // A stale (lower-height) report never downgrades the entry.
  cache.report(hash, make_diag(9, DbConflictCode::kDbConflictExclusiveLock), 9);

  auto found = cache.lookup(hash);
  ASSERT_TRUE(found.has_value());
  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  EXPECT_EQ(found->attempt_height, 12);
  EXPECT_EQ(found->conflict.code, DbConflictCode::kDbConflictOverflow);
  // NOLINTEND(bugprone-unchecked-optional-access)
  EXPECT_EQ(cache.size(), 1);
}

TEST(RecentTxFailureCacheTest, ExpiresExactlyAtDelta)
{
  RecentTxFailureCache cache(/*expiry_delta=*/5);
  const auto hash = make_hash(0x01);
  cache.report(hash, make_diag(100, DbConflictCode::kDbConflictValueMismatch),
               100);

  cache.prune(104);
  EXPECT_TRUE(cache.lookup(hash).has_value());
  cache.prune(105); // 100 + 5 <= 105 -> expired
  EXPECT_FALSE(cache.lookup(hash).has_value());
  EXPECT_EQ(cache.size(), 0);
}

TEST(RecentTxFailureCacheTest, ReReportExtendsLifetime)
{
  RecentTxFailureCache cache(/*expiry_delta=*/5);
  const auto hash = make_hash(0x01);
  cache.report(hash, make_diag(100, DbConflictCode::kDbConflictValueMismatch),
               100);
  cache.report(hash, make_diag(102, DbConflictCode::kDbConflictValueMismatch),
               102);

  // The height-100 expiry row fires at 105, but the entry now belongs to
  // attempt 102 and must survive until 107.
  cache.prune(105);
  EXPECT_TRUE(cache.lookup(hash).has_value());
  cache.prune(106);
  EXPECT_TRUE(cache.lookup(hash).has_value());
  cache.prune(107);
  EXPECT_FALSE(cache.lookup(hash).has_value());
}

TEST(RecentTxFailureCacheTest, SameHeightReReportUpdatesPayloadAndExpiresOnce)
{
  RecentTxFailureCache cache(/*expiry_delta=*/5);
  const auto hash = make_hash(0x01);
  cache.report(hash, make_diag(100, DbConflictCode::kDbConflictValueMismatch),
               100);
  // Re-report at the SAME attempt height with a different conflict code: the
  // payload updates in place and no second expiry row is created.
  cache.report(hash, make_diag(100, DbConflictCode::kDbConflictOverflow), 100);

  auto found = cache.lookup(hash);
  ASSERT_TRUE(found.has_value());
  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  EXPECT_EQ(found->attempt_height, 100);
  EXPECT_EQ(found->conflict.code, DbConflictCode::kDbConflictOverflow);
  // NOLINTEND(bugprone-unchecked-optional-access)
  EXPECT_EQ(cache.size(), 1);

  // A single expiry row at 100 + 5 expires it exactly once; no duplicate row
  // keeps it alive past the delta.
  cache.prune(104);
  EXPECT_TRUE(cache.lookup(hash).has_value());
  cache.prune(105);
  EXPECT_FALSE(cache.lookup(hash).has_value());
  EXPECT_EQ(cache.size(), 0);
}

TEST(RecentTxFailureCacheTest, CapDropsNewReportsButUpdatesExisting)
{
  RecentTxFailureCache cache(/*expiry_delta=*/5, /*max_entries=*/2);
  cache.report(make_hash(0x01),
               make_diag(100, DbConflictCode::kDbConflictValueMismatch), 100);
  cache.report(make_hash(0x02),
               make_diag(100, DbConflictCode::kDbConflictValueMismatch), 100);
  // Full: a report for a NEW hash is dropped...
  cache.report(make_hash(0x03),
               make_diag(100, DbConflictCode::kDbConflictValueMismatch), 100);
  EXPECT_EQ(cache.size(), 2);
  EXPECT_FALSE(cache.lookup(make_hash(0x03)).has_value());
  // ...but a re-report of an existing hash still updates it.
  cache.report(make_hash(0x01),
               make_diag(101, DbConflictCode::kDbConflictOverflow), 101);
  auto found = cache.lookup(make_hash(0x01));
  ASSERT_TRUE(found.has_value());
  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  EXPECT_EQ(found->attempt_height, 101);
  // NOLINTEND(bugprone-unchecked-optional-access)
}

TEST(RecentTxFailureCacheTest, PruneOnEmptyCacheIsNoop)
{
  RecentTxFailureCache cache;
  cache.prune(1000);
  EXPECT_EQ(cache.size(), 0);
}

TEST(RecentTxFailureCacheTest, RejectsImplausibleAttemptHeights)
{
  RecentTxFailureCache cache;

  // A far-future attempt height (beyond the reporting watermark plus slack)
  // could never satisfy the expiry test and would occupy a cap slot forever;
  // it must be rejected outright.
  cache.report(make_hash(0x01),
               make_diag(BlockHeight{1} << 62,
                         DbConflictCode::kDbConflictValueMismatch),
               /*watermark=*/100);
  EXPECT_FALSE(cache.lookup(make_hash(0x01)).has_value());
  EXPECT_EQ(cache.size(), 0);

  // Negative heights are malformed wire input; reject.
  cache.report(make_hash(0x02),
               make_diag(-1, DbConflictCode::kDbConflictValueMismatch),
               /*watermark=*/100);
  EXPECT_FALSE(cache.lookup(make_hash(0x02)).has_value());

  // A fresh or lagging indexer legitimately sees attempt heights well ahead
  // of its own watermark; those must still be accepted.
  cache.report(make_hash(0x03),
               make_diag(100, DbConflictCode::kDbConflictValueMismatch),
               /*watermark=*/0);
  EXPECT_TRUE(cache.lookup(make_hash(0x03)).has_value());
}

} // namespace hogd
