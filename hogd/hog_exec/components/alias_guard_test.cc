#include <gtest/gtest.h>

#include "hogd/base/types.h"
#include "hogd/hog_exec/components/alias_guard.h"

namespace hogd {

TEST(AliasGuardTest, SingleRegistrationAlwaysSucceeds)
{
  AliasGuard guard;
  EXPECT_TRUE(guard.check_and_register(10, 20));
}

TEST(AliasGuardTest, TwoNonOverlappingRangesSucceed)
{
  AliasGuard guard;
  const uintptr_t start = 100;
  EXPECT_TRUE(guard.check_and_register(start, 16));
  EXPECT_TRUE(guard.check_and_register(start + 16, 16));
  EXPECT_TRUE(guard.check());
}

TEST(AliasGuardTest, TwoIdenticalRangesFail)
{
  AliasGuard guard;
  const uintptr_t start = 100;
  EXPECT_TRUE(guard.check_and_register(start, 16));
  EXPECT_FALSE(guard.check_and_register(start, 16));
}

TEST(AliasGuardTest, PartialOverlapFails)
{
  AliasGuard guard;
  uintptr_t start = 100;

  EXPECT_TRUE(guard.check_and_register(start, 16));
  EXPECT_FALSE(guard.check_and_register(start + 8, 16));
  EXPECT_FALSE(guard.check_and_register(start - 8, 16));
}

TEST(AliasGuardTest, ContainedRangeFails)
{
  AliasGuard guard;
  uintptr_t start = 100;
  EXPECT_TRUE(guard.check_and_register(start, 32));
  EXPECT_FALSE(guard.check_and_register(start + 8, 8));
  EXPECT_FALSE(guard.check_and_register(start - 8, 64));
}

TEST(AliasGuardTest, AdjacentRangesSucceed)
{
  AliasGuard guard;
  EXPECT_TRUE(guard.check_and_register(100, 16));
  EXPECT_TRUE(guard.check_and_register(116, 16));
}

// Zero-length ranges under the half-open overlap predicate
// (start < prev.end && prev.start < end): a zero-length range strictly
// inside a prior range aliases; at the prior range's exact start or end it
// does not. The same holds with the registration order reversed. These
// semantics are guest-reachable (any zero-length span argument), so they are
// consensus-observable and must not drift.
TEST(AliasGuardTest, ZeroLengthInsidePriorRangeFails)
{
  AliasGuard guard;
  EXPECT_TRUE(guard.check_and_register(100, 16));
  EXPECT_FALSE(guard.check_and_register(108, 0));
  EXPECT_FALSE(guard.check());
}

TEST(AliasGuardTest, ZeroLengthAtPriorRangeEdgesSucceeds)
{
  AliasGuard guard;
  EXPECT_TRUE(guard.check_and_register(100, 16));
  EXPECT_TRUE(guard.check_and_register(100, 0));
  EXPECT_TRUE(guard.check_and_register(116, 0));
  EXPECT_TRUE(guard.check());
}

TEST(AliasGuardTest, NonzeroRangeOverZeroLengthPriorFails)
{
  AliasGuard guard;
  EXPECT_TRUE(guard.check_and_register(108, 0));
  EXPECT_FALSE(guard.check_and_register(100, 16));
}

TEST(AliasGuardTest, NonzeroRangeTouchingZeroLengthPriorSucceeds)
{
  AliasGuard guard;
  EXPECT_TRUE(guard.check_and_register(100, 0));
  EXPECT_TRUE(guard.check_and_register(100, 16));
  EXPECT_TRUE(guard.check());
}

TEST(AliasGuardTest, TwoZeroLengthRangesAtSamePointSucceed)
{
  AliasGuard guard;
  EXPECT_TRUE(guard.check_and_register(108, 0));
  EXPECT_TRUE(guard.check_and_register(108, 0));
  EXPECT_TRUE(guard.check());
}

TEST(AliasGuardTest, MultipleNonOverlappingRegistrationsSucceed)
{
  AliasGuard guard;
  EXPECT_TRUE(guard.check_and_register(100, 16));
  EXPECT_TRUE(guard.check_and_register(116, 16));
  EXPECT_TRUE(guard.check_and_register(132, 16));
  EXPECT_TRUE(guard.check_and_register(148, 16));
  EXPECT_TRUE(guard.check_and_register(164, 16));
  EXPECT_TRUE(guard.check_and_register(180, 16));
  EXPECT_TRUE(guard.check_and_register(196, 16));
  EXPECT_TRUE(guard.check_and_register(212, 16));
}

} // namespace hogd
