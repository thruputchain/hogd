#include <gtest/gtest.h>

#include "auth_claim.hh"
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
  AuthClaim a(AuthClaimType::kInvokedAddressSignature);
  a.invoked_address_signature_id() = 1;
  auto b = roundtrip_auth_claim(a);
  ASSERT_EQ(a, b);

  a.claim_type(AuthClaimType::kInvokedAddressSignedWeight);
  a.invoked_address_signed_weight().accepted_keys = 1;
  a.invoked_address_signed_weight().required_weight = 1;
  b = roundtrip_auth_claim(a);
  ASSERT_TRUE(b.has_value());
  ASSERT_EQ(a, *b);

  a.claim_type(AuthClaimType::kOtherAddressSignature);
  a.other_address_signature_id().address = ContractAddress{1};
  a.other_address_signature_id().key_id = 1;
  b = roundtrip_auth_claim(a);
  ASSERT_TRUE(b.has_value());
  ASSERT_EQ(a, *b);

  a.claim_type(AuthClaimType::kOtherAddressSignedWeight);
  a.other_address_signed_weight().address = ContractAddress{123};
  a.other_address_signed_weight().accepted_keys = 1;
  a.other_address_signed_weight().required_weight = 1;
  b = roundtrip_auth_claim(a);
  ASSERT_TRUE(b.has_value());
  ASSERT_EQ(a, *b);
}

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
