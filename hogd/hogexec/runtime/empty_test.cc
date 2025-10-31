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

} // namespace hogd
