#include <gtest/gtest.h>

#include <limits>

#include "auth_claim.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/ffi_helpers.h"
#include "hogd/rust/contract_types/bindings.h"
#include "types.hh"

namespace hogd {

// Roundtrip tests on the FFI helper types

namespace {

auto
roundtrip_auth_claim(AuthClaim const &auth_claim)
{
  auto ffi_auth_claim = FFIHelperType<generated::AuthClaim>::to_ffi(auth_claim);
  return FFIHelperType<generated::AuthClaim>::to_cpp(ffi_auth_claim);
}

} // namespace

// NOLINTBEGIN(bugprone-unchecked-optional-access)
TEST(FFIHelpers, AuthClaimRoundtrip)
{
  AuthClaim a(AuthClaimType::kSourceAddressSignedWeight);
  a.accepted_keys() = 1;
  auto b = roundtrip_auth_claim(a);
  ASSERT_TRUE(b.has_value());
  ASSERT_EQ(a, *b);

  // Test with different bitmap values
  a.accepted_keys() = 0x15; // bits 0, 2, 4
  b = roundtrip_auth_claim(a);
  ASSERT_TRUE(b.has_value());
  ASSERT_EQ(a, *b);
}

TEST(FFIHelpers, AuthClaimInvalid)
{
  generated::AuthClaim ffi_auth_claim;
  ffi_auth_claim.tag = static_cast<generated::AuthClaim::Tag>(-1);
  auto b = FFIHelperType<generated::AuthClaim>::to_cpp(ffi_auth_claim);
  ASSERT_FALSE(b.has_value());
}

// Verify to_cpp returns nullopt for various out-of-range tag values,
// simulating corrupted or adversarial FFI data from untrusted WASM memory.
TEST(FFIHelpers, AuthClaimInvalidTags)
{
  for (int tag_val :
       {-2, 100, 255, 1000, std::numeric_limits<int32_t>::max()}) {
    generated::AuthClaim ffi_auth_claim{};
    ffi_auth_claim.tag = static_cast<generated::AuthClaim::Tag>(tag_val);
    auto result = FFIHelperType<generated::AuthClaim>::to_cpp(ffi_auth_claim);
    EXPECT_FALSE(result.has_value())
        << "Expected nullopt for invalid tag value " << tag_val;
  }
}

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
