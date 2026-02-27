#include <gtest/gtest.h>

#include "hogd/base/types.h"
#include "hogd/hogexec/components/alias_guard.h"

namespace hogd {

TEST(AliasGuardTest, SingleRegistrationAlwaysSucceeds)
{
  AliasGuard guard;
  uint8_t buf[16] = {};
  EXPECT_TRUE(guard.check_and_register(buf, sizeof(buf)));
}

TEST(AliasGuardTest, TwoNonOverlappingRangesSucceed)
{
  AliasGuard guard;
  uint8_t buf[32] = {};
  EXPECT_TRUE(guard.check_and_register(buf, 16));
  EXPECT_TRUE(guard.check_and_register(buf + 16, 16));
}

TEST(AliasGuardTest, TwoIdenticalRangesFail)
{
  AliasGuard guard;
  uint8_t buf[16] = {};
  EXPECT_TRUE(guard.check_and_register(buf, sizeof(buf)));
  EXPECT_FALSE(guard.check_and_register(buf, sizeof(buf)));
}

TEST(AliasGuardTest, PartialOverlapFails)
{
  AliasGuard guard;
  uint8_t buf[32] = {};
  EXPECT_TRUE(guard.check_and_register(buf, 16));
  EXPECT_FALSE(guard.check_and_register(buf + 8, 16));
  EXPECT_FALSE(guard.check_and_register(buf - 8, 16));
}

TEST(AliasGuardTest, ContainedRangeFails)
{
  AliasGuard guard;
  uint8_t buf[32] = {};
  EXPECT_TRUE(guard.check_and_register(buf, 32));
  EXPECT_FALSE(guard.check_and_register(buf + 8, 8));
  EXPECT_FALSE(guard.check_and_register(buf - 8, 64));
}

TEST(AliasGuardTest, AdjacentRangesSucceed)
{
  AliasGuard guard;
  uint8_t buf[32] = {};
  EXPECT_TRUE(guard.check_and_register(buf, 16));
  EXPECT_TRUE(guard.check_and_register(buf + 16, 16));
}

TEST(AliasGuardTest, MultipleNonOverlappingRegistrationsSucceed)
{
  AliasGuard guard;
  uint8_t buf[128] = {};
  EXPECT_TRUE(guard.check_and_register(buf, 16));
  EXPECT_TRUE(guard.check_and_register(buf + 16, 16));
  EXPECT_TRUE(guard.check_and_register(buf + 32, 16));
  EXPECT_TRUE(guard.check_and_register(buf + 48, 16));
  EXPECT_TRUE(guard.check_and_register(buf + 64, 16));
  EXPECT_TRUE(guard.check_and_register(buf + 80, 16));
  EXPECT_TRUE(guard.check_and_register(buf + 96, 16));
  EXPECT_TRUE(guard.check_and_register(buf + 112, 16));
}

} // namespace hogd
