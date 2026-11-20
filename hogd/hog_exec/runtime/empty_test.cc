#include <gtest/gtest.h>

#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "rpc_types.hh"

namespace hogd {

// We've had a number of bugs in which an empty test triggers some kind of
// shutdown stall (possibly due to no or few rpcs being sent).
// The failures are sporadic, so this test repeatedly turns on and off the
// test harness to try and trigger the bug.
TEST(EmptyTest, RepeatedInstantiate)
{
  for (int i = 0; i < 20; i++) {
    NativeContractTestFramework test_framework(
        false, BlockExecutionMode::kBlockExecutionModeProposal);
  }
}

// NOLINTNEXTLINE(misc-use-internal-linkage)
struct EmptyTestStruct : public NativeContractTestFramework,
                         public ::testing::Test {
  EmptyTestStruct()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal)
  {}
};

TEST_F(EmptyTestStruct, Basic1)
{
  // Do nothing at all
}

TEST_F(EmptyTestStruct, Basic2)
{
  // Do nothing at all
}

TEST_F(EmptyTestStruct, Basic3)
{
  // Do nothing at all
}

TEST_F(EmptyTestStruct, Basic4)
{
  // Do nothing at all
}

TEST_F(EmptyTestStruct, Basic5)
{
  // Do nothing at all
}

TEST_F(EmptyTestStruct, Basic6)
{
  // Do nothing at all
}

TEST_F(EmptyTestStruct, Basic7)
{
  // Do nothing at all
}

TEST_F(EmptyTestStruct, Basic8)
{
  // Do nothing at all
}

TEST_F(EmptyTestStruct, Basic9)
{
  // Do nothing at all
}

TEST_F(EmptyTestStruct, Basic10)
{
  // Do nothing at all
}

} // namespace hogd
