#include <gtest/gtest.h>

#include "hogd/hog_exec/components/contract_result.h"

namespace hogd {

namespace {

// Validates the hard-coded `kLibraryExceptionSelector` against the SDK
// derivation (charset-independent: `calculate_selector` uses u8 literals).
TEST(ContractResultTest, LibraryExceptionSelectorMatchesDerivation)
{
  EXPECT_EQ(kLibraryExceptionSelector,
            calculate_selector(u8"1", u8"sdk", u8"LibraryExn"));
}

} // namespace

} // namespace hogd
