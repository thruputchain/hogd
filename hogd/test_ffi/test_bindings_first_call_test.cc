// Regression coverage for the standalone test_ffi auth verifier when it is the
// very first test_ffi API call in a process.
//
// `verify_signed_transaction_auth()` does not require a prior stateful call
// (e.g. create_db / mock_syscall_entrypoint) to install a logger. An invalid
// signature reaches a debug-build `trace(...)` inside auth verification, so if
// the wrapper does not initialize logging first, that trace dereferences the
// still-null `detail::logger` and crashes an honest C API caller (issue #2464).
//
// This binary deliberately links against GTest::gtest_main rather than the
// custom main() used by test_ffi_test, because that main() calls hogd_init()
// before any test runs and would mask the uninitialized-logger path.

#include <cstdint>
#include <utility>

#include <gtest/gtest.h>

#include "address_auth.hh"
#include "auth_claim.hh"
#include "hogd/auth_claim/signature_preimage.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/test_ffi/test_bindings.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/serialize.h"
#include "x23/x23.h"

namespace hogd {

namespace {

std::pair<PublicKey, SecretKey>
make_key_pair()
{
  auto [pk, sk] = ed25519_keygen();
  PublicKey pk_wrapper(CryptographicScheme::Ed25519);
  pk_wrapper.ed25519() = pk;
  SecretKey sk_wrapper(CryptographicScheme::Ed25519);
  sk_wrapper.ed25519() = sk;
  return {pk_wrapper, sk_wrapper};
}

constexpr NetworkHash kTestNetworkHash{0x5a};

// NOLINTBEGIN(bugprone-unchecked-optional-access)
DecoratedSignature
make_signature(SecretKey sk, Transaction const &tx, uint32_t public_key_id)
{
  Signature sig(CryptographicScheme::Ed25519);
  sig.ed25519() =
      ed25519_sign(sk.ed25519(),
                   transaction_signature_preimage(kTestNetworkHash, tx))
          .value();
  return {.public_key_id = public_key_id, .signature = sig};
}
// NOLINTEND(bugprone-unchecked-optional-access)

auto
make_source_auth_data(x23::BufView serialized_auth_data) -> FFISourceAuthData
{
  return {.data = serialized_auth_data.data(),
          .len = static_cast<uint32_t>(serialized_auth_data.size())};
}

} // namespace

// Calling the standalone verifier first, with a well-formed transaction whose
// signature does not match the configured key, must return the structured
// wrong-signature auth error instead of crashing on a null logger.
TEST(TestBindingsFirstCallTest,
     VerifyAuthInvalidSignatureReturnsStructuredError)
{
  auto signing_key_pair = make_key_pair();
  auto configured_key_pair = make_key_pair();

  AuthClaim claim(AuthClaimType::kSourceAddressAuthData);
  claim.source_address_claim().allowed_keys = 1 << 0;
  claim.source_address_claim().required_weight = 1;

  Transaction tx{};
  tx.invocation.invoked_address = ContractAddress{1};
  tx.invocation.gas_limit = 100'000;
  tx.auth_claim = claim;

  DecoratedSignature sig =
      make_signature(signing_key_pair.second, tx, /*public_key_id=*/0);
  SignedTransaction stx{.tx = tx, .signatures = {sig}};

  // Configured key differs from the signing key, so signature verification
  // fails and the invalid-signature trace path is exercised.
  AddressAuthData source_auth_data{
      .public_keys = {x23::Ptr<PublicKeyConfig>(new PublicKeyConfig{
          .key = configured_key_pair.first, .max_weight = Weight{1}})},
      .required_weight = 1};

  auto serialized_stx = x23::serialize(stx);
  auto serialized_auth_data = x23::serialize(source_auth_data);

  VerifyAuthResult result = verify_signed_transaction_auth(
      {.data = serialized_stx.data(),
       .len = static_cast<uint32_t>(serialized_stx.size())},
      make_source_auth_data(serialized_auth_data), kTestNetworkHash.data());

  EXPECT_EQ(result.ffi_error_code, kTestFFIErrorCodeNone);
  EXPECT_EQ(result.auth_error_code,
            std::to_underlying(
                AuthVerifyErrorCode::kSignatureVerificationFailed));
}

} // namespace hogd
