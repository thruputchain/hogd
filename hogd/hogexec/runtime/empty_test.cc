#include <gtest/gtest.h>

#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "rpc_types.hh"

namespace hogd {

// We've had a number of bugs in which an empty test triggers some kind of
// shutdown stall (possibly due to no or few rpcs being sent).
// The failures are sporadic, so this test repeatedly turns on and off the
// test harness to try and trigger the bug.
TEST(EmptyTest, RepeatedInstantiate)
{
  for (int i = 0; i < 20; i++) {
    NativeContractTestFramework test_framework_mock(
        false, BlockExecutionMode::kBlockExecutionModeProposal,
        TestDB::kMockInMemory);
    NativeContractTestFramework test_framework_hog(
        false, BlockExecutionMode::kBlockExecutionModeProposal, TestDB::kHogDB);
  }
}

// NOLINTNEXTLINE(misc-use-internal-linkage)
struct EmptyTestStruct : public NativeContractTestFramework,
                         public ::testing::TestWithParam<TestDB> {
  EmptyTestStruct()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}
};

TEST_P(EmptyTestStruct, Basic1)
{
  // Do nothing at all
}

TEST_P(EmptyTestStruct, Basic2)
{
  // Do nothing at all
}

TEST_P(EmptyTestStruct, Basic3)
{
  // Do nothing at all
}

TEST_P(EmptyTestStruct, Basic4)
{
  // Do nothing at all
}

TEST_P(EmptyTestStruct, Basic5)
{
  // Do nothing at all
}

TEST_P(EmptyTestStruct, Basic6)
{
  // Do nothing at all
}

TEST_P(EmptyTestStruct, Basic7)
{
  // Do nothing at all
}

TEST_P(EmptyTestStruct, Basic8)
{
  // Do nothing at all
}

TEST_P(EmptyTestStruct, Basic9)
{
  // Do nothing at all
}

TEST_P(EmptyTestStruct, Basic10)
{
  // Do nothing at all
}

INSTANTIATE_TEST_SUITE_P(TestDBs, EmptyTestStruct,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

} // namespace hogd
