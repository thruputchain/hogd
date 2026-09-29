#include <gtest/gtest.h>

#include "hogd/hog_exec/components/contract_result.h"

namespace hogd {

namespace {

// `kLibraryExceptionSelector` is the SHA256("1::sdk::LibraryExn") prefix that
// the contract SDK derives for the `LibraryExn` selector. Consensus uses the
// hard-coded constant directly, so validate it here (on the canonical UTF-8
// build) instead of recomputing it on the runtime abort-decode path, where the
// result would depend on the implementation-defined execution character set.
TEST(ContractResultTest, LibraryExceptionSelectorMatchesDerivation)
{
  EXPECT_EQ(kLibraryExceptionSelector,
            calculate_selector("1", "sdk", "LibraryExn"));
}

} // namespace

} // namespace hogd
