#include <gtest/gtest.h>

#include "hogd/base/types.h"
#include "hogd/hog_exec/components/alias_guard.h"

namespace hogd {

TEST(AliasGuardTest, SingleRegistrationAlwaysSucceeds)
{
  AliasGuard guard;
  EXPECT_TRUE(guard.check_and_register_output(10, 20));
}

TEST(AliasGuardTest, TwoNonOverlappingRangesSucceed)
{
  AliasGuard guard;
  const uintptr_t start = 100;
  EXPECT_TRUE(guard.check_and_register_output(start, 16));
  EXPECT_TRUE(guard.check_and_register_output(start + 16, 16));
  EXPECT_TRUE(guard.check());
}

TEST(AliasGuardTest, TwoIdenticalRangesFail)
{
  AliasGuard guard;
  const uintptr_t start = 100;
  EXPECT_TRUE(guard.check_and_register_output(start, 16));
  EXPECT_FALSE(guard.check_and_register_output(start, 16));
}

TEST(AliasGuardTest, PartialOverlapFails)
{
  AliasGuard guard;
  uintptr_t start = 100;

  EXPECT_TRUE(guard.check_and_register_output(start, 16));
  EXPECT_FALSE(guard.check_and_register_output(start + 8, 16));
  EXPECT_FALSE(guard.check_and_register_output(start - 8, 16));
}

TEST(AliasGuardTest, ContainedRangeFails)
{
  AliasGuard guard;
  uintptr_t start = 100;
  EXPECT_TRUE(guard.check_and_register_output(start, 32));
  EXPECT_FALSE(guard.check_and_register_output(start + 8, 8));
  EXPECT_FALSE(guard.check_and_register_output(start - 8, 64));
}

TEST(AliasGuardTest, AdjacentRangesSucceed)
{
  AliasGuard guard;
  EXPECT_TRUE(guard.check_and_register_output(100, 16));
  EXPECT_TRUE(guard.check_and_register_output(116, 16));
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
  EXPECT_TRUE(guard.check_and_register_output(100, 16));
  EXPECT_FALSE(guard.check_and_register_output(108, 0));
  EXPECT_FALSE(guard.check());
}

TEST(AliasGuardTest, ZeroLengthAtPriorRangeEdgesSucceeds)
{
  AliasGuard guard;
  EXPECT_TRUE(guard.check_and_register_output(100, 16));
  EXPECT_TRUE(guard.check_and_register_output(100, 0));
  EXPECT_TRUE(guard.check_and_register_output(116, 0));
  EXPECT_TRUE(guard.check());
}

TEST(AliasGuardTest, NonzeroRangeOverZeroLengthPriorFails)
{
  AliasGuard guard;
  EXPECT_TRUE(guard.check_and_register_output(108, 0));
  EXPECT_FALSE(guard.check_and_register_output(100, 16));
}

TEST(AliasGuardTest, NonzeroRangeTouchingZeroLengthPriorSucceeds)
{
  AliasGuard guard;
  EXPECT_TRUE(guard.check_and_register_output(100, 0));
  EXPECT_TRUE(guard.check_and_register_output(100, 16));
  EXPECT_TRUE(guard.check());
}

TEST(AliasGuardTest, TwoZeroLengthRangesAtSamePointSucceed)
{
  AliasGuard guard;
  EXPECT_TRUE(guard.check_and_register_output(108, 0));
  EXPECT_TRUE(guard.check_and_register_output(108, 0));
  EXPECT_TRUE(guard.check());
}

// The tests above register outputs, which may overlap nothing. An input may
// overlap another input in any way: the host reads both and writes neither.
TEST(AliasGuardTest, IdenticalInputsSucceed)
{
  AliasGuard guard;
  EXPECT_TRUE(guard.check_and_register_input(100, 16));
  EXPECT_TRUE(guard.check_and_register_input(100, 16));
  EXPECT_TRUE(guard.check());
}

TEST(AliasGuardTest, PartiallyOverlappingInputsSucceed)
{
  AliasGuard guard;
  EXPECT_TRUE(guard.check_and_register_input(100, 16));
  EXPECT_TRUE(guard.check_and_register_input(108, 16));
  EXPECT_TRUE(guard.check_and_register_input(92, 16));
  EXPECT_TRUE(guard.check());
}

TEST(AliasGuardTest, ContainedInputsSucceed)
{
  AliasGuard guard;
  EXPECT_TRUE(guard.check_and_register_input(100, 32));
  EXPECT_TRUE(guard.check_and_register_input(108, 8));
  EXPECT_TRUE(guard.check_and_register_input(92, 64));
  EXPECT_TRUE(guard.check());
}

TEST(AliasGuardTest, ZeroLengthInputInsidePriorInputSucceeds)
{
  AliasGuard guard;
  EXPECT_TRUE(guard.check_and_register_input(100, 16));
  EXPECT_TRUE(guard.check_and_register_input(108, 0));
  EXPECT_TRUE(guard.check());
}

TEST(AliasGuardTest, InputOverZeroLengthPriorInputSucceeds)
{
  AliasGuard guard;
  EXPECT_TRUE(guard.check_and_register_input(108, 0));
  EXPECT_TRUE(guard.check_and_register_input(100, 16));
  EXPECT_TRUE(guard.check());
}

// An output still may not overlap an input, whichever is registered first.
TEST(AliasGuardTest, InputOverlappingPriorOutputFails)
{
  AliasGuard guard;
  EXPECT_TRUE(guard.check_and_register_output(100, 16));
  EXPECT_FALSE(guard.check_and_register_input(100, 16));
  EXPECT_FALSE(guard.check_and_register_input(108, 16));
  EXPECT_FALSE(guard.check_and_register_input(92, 16));
  EXPECT_FALSE(guard.check_and_register_input(104, 4));
  EXPECT_FALSE(guard.check());
}

TEST(AliasGuardTest, OutputOverlappingPriorInputFails)
{
  AliasGuard guard;
  EXPECT_TRUE(guard.check_and_register_input(100, 16));
  EXPECT_FALSE(guard.check_and_register_output(100, 16));
  EXPECT_FALSE(guard.check_and_register_output(108, 16));
  EXPECT_FALSE(guard.check_and_register_output(92, 16));
  EXPECT_FALSE(guard.check_and_register_output(104, 4));
  EXPECT_FALSE(guard.check());
}

// Sharing bytes with another input does not hide an input from the outputs
// that follow.
TEST(AliasGuardTest, OutputOverlappingOneOfTwoSharedInputsFails)
{
  AliasGuard guard;
  EXPECT_TRUE(guard.check_and_register_input(100, 16));
  EXPECT_TRUE(guard.check_and_register_input(108, 16));
  // [116, 124) is inside the second input only.
  EXPECT_FALSE(guard.check_and_register_output(116, 8));
  EXPECT_FALSE(guard.check());
}

TEST(AliasGuardTest, InputsAndOutputsThatOnlyTouchSucceed)
{
  AliasGuard guard;
  EXPECT_TRUE(guard.check_and_register_input(100, 16));
  EXPECT_TRUE(guard.check_and_register_output(116, 16));
  EXPECT_TRUE(guard.check_and_register_input(132, 16));
  EXPECT_TRUE(guard.check_and_register_input(100, 16));
  EXPECT_TRUE(guard.check());
}

// The zero-length rule is the same one between an input and an output as
// between two outputs: strictly inside conflicts, at an edge does not.
TEST(AliasGuardTest, ZeroLengthInputInsidePriorOutputFails)
{
  AliasGuard guard;
  EXPECT_TRUE(guard.check_and_register_output(100, 16));
  EXPECT_FALSE(guard.check_and_register_input(108, 0));
  EXPECT_FALSE(guard.check());
}

TEST(AliasGuardTest, ZeroLengthInputAtPriorOutputEdgesSucceeds)
{
  AliasGuard guard;
  EXPECT_TRUE(guard.check_and_register_output(100, 16));
  EXPECT_TRUE(guard.check_and_register_input(100, 0));
  EXPECT_TRUE(guard.check_and_register_input(116, 0));
  EXPECT_TRUE(guard.check());
}

TEST(AliasGuardTest, ZeroLengthOutputInsidePriorInputFails)
{
  AliasGuard guard;
  EXPECT_TRUE(guard.check_and_register_input(100, 16));
  EXPECT_FALSE(guard.check_and_register_output(108, 0));
  EXPECT_FALSE(guard.check());
}

TEST(AliasGuardTest, ZeroLengthOutputAtPriorInputEdgesSucceeds)
{
  AliasGuard guard;
  EXPECT_TRUE(guard.check_and_register_input(100, 16));
  EXPECT_TRUE(guard.check_and_register_output(100, 0));
  EXPECT_TRUE(guard.check_and_register_output(116, 0));
  EXPECT_TRUE(guard.check());
}

TEST(AliasGuardTest, OutputOverZeroLengthPriorInputFails)
{
  AliasGuard guard;
  EXPECT_TRUE(guard.check_and_register_input(108, 0));
  EXPECT_FALSE(guard.check_and_register_output(100, 16));
}

TEST(AliasGuardTest, InputOverZeroLengthPriorOutputFails)
{
  AliasGuard guard;
  EXPECT_TRUE(guard.check_and_register_output(108, 0));
  EXPECT_FALSE(guard.check_and_register_input(100, 16));
}

// A conflict is remembered: nothing registered afterwards clears it.
TEST(AliasGuardTest, LaterRegistrationsDoNotClearAConflict)
{
  AliasGuard guard;
  EXPECT_TRUE(guard.check_and_register_output(100, 16));
  EXPECT_FALSE(guard.check_and_register_input(100, 16));
  EXPECT_TRUE(guard.check_and_register_input(200, 16));
  EXPECT_TRUE(guard.check_and_register_input(200, 16));
  EXPECT_TRUE(guard.check_and_register_output(300, 16));
  EXPECT_FALSE(guard.check());
}

// A syscall has seven argument words, so at most seven memory arguments; all
// of them may be one shared input.
TEST(AliasGuardTest, SevenIdenticalInputsSucceed)
{
  AliasGuard guard;
  for (int i = 0; i < 7; ++i) {
    EXPECT_TRUE(guard.check_and_register_input(100, 16));
  }
  EXPECT_TRUE(guard.check());
}

TEST(AliasGuardTest, MultipleNonOverlappingRegistrationsSucceed)
{
  AliasGuard guard;
  EXPECT_TRUE(guard.check_and_register_output(100, 16));
  EXPECT_TRUE(guard.check_and_register_output(116, 16));
  EXPECT_TRUE(guard.check_and_register_output(132, 16));
  EXPECT_TRUE(guard.check_and_register_output(148, 16));
  EXPECT_TRUE(guard.check_and_register_output(164, 16));
  EXPECT_TRUE(guard.check_and_register_output(180, 16));
  EXPECT_TRUE(guard.check_and_register_output(196, 16));
  EXPECT_TRUE(guard.check_and_register_output(212, 16));
}

} // namespace hogd
