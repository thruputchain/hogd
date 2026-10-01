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

TEST(ContractResultTest, RawAbortPreservesKnownSelectorAndOpaqueBody)
{
  auto encoded = x23::serialize_cxs(kLibraryExceptionSelector);
  ASSERT_TRUE(encoded);
  ReturnBuffer wire(encoded->begin(), encoded->end());
  wire.push_back(0xff); // Deliberately not a decodable LibraryExn body.
  auto error = ContractErrorType<>::from_raw_abort(wire);
  auto serialized = error.serialize();
  ASSERT_TRUE(serialized);
  EXPECT_EQ(*serialized, wire);
}

} // namespace

} // namespace hogd
