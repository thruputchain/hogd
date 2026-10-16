#include "verify.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "address_auth.hh"
#include "auth_claim.hh"
#include "hogd/auth_claim/signature_preimage.h"
#include "hogd/auth_claim/status.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/crypto/crypto.h"
#include "hogd/crypto/domain_separators.h"
#include "hogd/crypto/mldsa.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

void
Ed25519SignatureVerifier::add_signature(const Ed25519PublicKey &public_key,
                                        const Ed25519Signature &signature,
                                        x23::BufView signature_preimage)
{
  public_keys_.push_back(public_key);
  signatures_.push_back(signature);
  signature_preimages_.emplace_back(signature_preimage.begin(),
                                    signature_preimage.end());
}

bool
Ed25519SignatureVerifier::verify()
{
  return ed25519_verify_batch(public_keys_, signatures_,
                              std::span<const x23::Buf>(signature_preimages_));
}

namespace {

constexpr bool
valid_pk_claim(size_t idx)
{
  return idx < kMaxSigners;
}

constexpr bool
valid_pk_bitmap(BitMap bitmap)
{
  // no pk has an index higher than kMaxSigners
  static_assert(kMaxSigners <= 32);
  return (bitmap & (0xFFFF'FFFF << kMaxSigners)) == 0;
}

[[nodiscard]] constexpr bool
bitmap_accepts_key(BitMap bitmap, size_t idx)
{
  if (idx >= 32) {
    return false;
  }
  return (bitmap & (uint32_t{1} << idx)) != 0;
}

static_assert(valid_pk_claim(0));
static_assert(valid_pk_claim(20));
static_assert(!valid_pk_claim(21));

static_assert(valid_pk_bitmap(0));
static_assert(!valid_pk_bitmap(0xFFFF'FFFF));
static_assert(!valid_pk_bitmap(1 << kMaxSigners));
static_assert(valid_pk_bitmap(1 << (kMaxSigners - 1)));

static_assert(!bitmap_accepts_key(0, 0));
static_assert(bitmap_accepts_key(0xF, 2));

bool
has_public_key(const AddressAuthData &auth_data, size_t idx)
{
  return idx < auth_data.public_keys.size() && auth_data.public_keys[idx];
}

// Verifies sig on signature_preimage under key. Ed25519 signatures go to
// signature_verifier, when there is one, to be checked as a batch later;
// ML-DSA has no batch verification, so ML-DSA signatures are checked here.
// A signature of a different scheme than the key never verifies.
AuthVerifyStatus<void>
verify_signature(const PublicKey &key, const Signature &sig,
                 x23::BufView signature_preimage,
                 Ed25519SignatureVerifier *signature_verifier)
{
  if (key.scheme() != sig.scheme()) {
    trace("Signature verification failed: signature scheme {} does not match "
          "key scheme {}",
          sig.scheme(), key.scheme());
    return AuthVerifyError{AuthVerifyErrorCode::kSignatureVerificationFailed};
  }

  bool ok = false;
  switch (key.scheme()) {
  case CryptographicScheme::Ed25519:
    if (signature_verifier) {
      signature_verifier->add_signature(key.ed25519(), sig.ed25519(),
                                        signature_preimage);
      return {};
    }
    ok = ed25519_verify(key.ed25519(), sig.ed25519(), signature_preimage);
    break;
  case CryptographicScheme::MlDsa65:
    // The preimage is already domain-separated, so the context is empty.
    ok = mldsa65_verify(*key.mldsa65(), *sig.mldsa65(), signature_preimage);
    break;
  default:
    warn("Unsupported cryptographic scheme: {}", key.scheme());
    return AuthVerifyError{AuthVerifyErrorCode::kAuthMalformed};
  }
  if (!ok) {
    trace("Signature verification failed");
    return AuthVerifyError{AuthVerifyErrorCode::kSignatureVerificationFailed};
  }
  return {};
}

AuthVerifyStatus<void>
verify_decorated_signature(const DecoratedSignature &dec_sig,
                           const AddressAuthData &auth_data,
                           x23::BufView signature_preimage,
                           Ed25519SignatureVerifier *signature_verifier)
{
  assert(has_public_key(auth_data, dec_sig.public_key_id));

  const PublicKeyConfigOpt &pk = auth_data.public_keys[dec_sig.public_key_id];
  assert(pk);

  return verify_signature(pk->key, dec_sig.signature, signature_preimage,
                          signature_verifier);
}

enum class VerificationType {
  kLightweightClaimedOnly,
  kLightweightClaimsValid,
};

constexpr bool
verify_weights_against_auth_data(VerificationType VerfType)
{
  return VerfType == VerificationType::kLightweightClaimsValid;
}

template<VerificationType VerfType>
AuthVerifyStatus<void>
verify_weight_key_claim_internal(
    BitMap accepted_keys, const AddressAuthData *auth_data,
    std::span<const DecoratedSignature> sigs)
{
  if (!valid_pk_bitmap(accepted_keys)) {
    return AuthVerifyError{AuthVerifyErrorCode::kAuthMalformed};
  }

  uint32_t prev_key_id = 0;
  bool have_prev_key_id = false;
  for (const DecoratedSignature &sig : sigs) {
    if (!bitmap_accepts_key(accepted_keys, sig.public_key_id)) {
      return AuthVerifyError{AuthVerifyErrorCode::kAuthMalformed};
    }
    if (have_prev_key_id && sig.public_key_id <= prev_key_id) {
      trace("Auth malformed: weighted-claim signer ids must be strictly "
            "increasing");
      return AuthVerifyError{AuthVerifyErrorCode::kAuthMalformed};
    }
    prev_key_id = sig.public_key_id;
    have_prev_key_id = true;
  }

  if constexpr (verify_weights_against_auth_data(VerfType)) {
    assert(auth_data);
    for (const DecoratedSignature &sig : sigs) {
      if (!has_public_key(*auth_data, sig.public_key_id)) {
        return AuthVerifyError{AuthVerifyErrorCode::kAuthDataMismatch};
      }
    }
  }

  return {};
}

// Verifies a kPubkeySignature claim: a single signature from a standalone
// public key, for an account whose address is the hash of the pubkey
// preimage and which has no AddressAuthData registered onchain (that
// precondition is enforced by the caller, which has the DB data).
//
// The preimage names the key by its hash; key is the one the signed
// transaction attaches (SignedTransaction::pubkey_claim_key), or nullptr if
// it attaches none.
AuthVerifyStatus<void>
verify_pubkey_claim_internal(const PubkeyAccountPreimage &preimage,
                             const PublicKey *key,
                             const ContractAddress &invoked_address,
                             std::span<const DecoratedSignature> sigs)
{
  if (sigs.size() != 1) {
    trace("Auth malformed: pubkey claim requires exactly one signature");
    return AuthVerifyError{AuthVerifyErrorCode::kAuthMalformed};
  }
  const DecoratedSignature &sig = sigs[0];
  if (sig.public_key_id != kDefaultNewAddressKeyID) {
    trace("Auth malformed: pubkey claim signature must use key id {}",
          kDefaultNewAddressKeyID);
    return AuthVerifyError{AuthVerifyErrorCode::kAuthMalformed};
  }
  if (key == nullptr) {
    trace("Auth malformed: pubkey claim without its public key");
    return AuthVerifyError{AuthVerifyErrorCode::kAuthMalformed};
  }
  if (key->scheme() != sig.signature.scheme()) {
    trace("Auth malformed: pubkey claim signature scheme {} does not match "
          "key scheme {}",
          sig.signature.scheme(), key->scheme());
    return AuthVerifyError{AuthVerifyErrorCode::kAuthMalformed};
  }
  std::optional<PublicKeyHash> key_hash = public_key_hash(*key);
  if (!key_hash.has_value()) {
    trace("Auth malformed: failed to serialize pubkey claim key");
    return AuthVerifyError{AuthVerifyErrorCode::kAuthMalformed};
  }
  if (*key_hash != preimage.pubkey_hash) {
    trace("Auth malformed: pubkey claim key does not match the claimed "
          "pubkey hash");
    return AuthVerifyError{AuthVerifyErrorCode::kAuthMalformed};
  }
  std::optional<ContractAddress> derived_address =
      pubkey_auth_address(preimage);
  if (!derived_address.has_value()) {
    trace("Auth malformed: failed to serialize pubkey preimage");
    return AuthVerifyError{AuthVerifyErrorCode::kAuthMalformed};
  }
  if (*derived_address != invoked_address) {
    trace("Auth malformed: pubkey preimage hash does not match the invoked "
          "address");
    return AuthVerifyError{AuthVerifyErrorCode::kAuthMalformed};
  }

  return {};
}

template<VerificationType VerfType>
AuthVerifyStatus<void>
verify_auth_claim_internal(AuthClaim const &claim,
                           PublicKey const *pubkey_claim_key,
                           ContractAddress const &invoked_address,
                           std::span<const DecoratedSignature> sigs,
                           const AddressAuthData *auth_data)
{
  switch (claim.claim_type()) {
  case AuthClaimType::kSourceAddressAuthData:
    if (pubkey_claim_key != nullptr) {
      trace("Auth malformed: pubkey claim key on a source address claim");
      return AuthVerifyError{AuthVerifyErrorCode::kAuthMalformed};
    }
    if constexpr (VerfType == VerificationType::kLightweightClaimedOnly) {
      assert(!auth_data);
    }
    else {
      assert(auth_data);
    }
    return verify_weight_key_claim_internal<VerfType>(
        claim.source_address_claim().allowed_keys, auth_data, sigs);
  case AuthClaimType::kPubkeySignature:
    // The no-auth-entry precondition is checked by the caller (it owns the
    // DB data); everything here is derivable from the transaction alone.
    return verify_pubkey_claim_internal(claim.pubkey_preimage(),
                                        pubkey_claim_key, invoked_address,
                                        sigs);
  default:
    // This kind of claim should not have been parseable
    panic("Unsupported auth claim type: {}", claim.claim_type());
  }
}

template<VerificationType VerfType>
AuthVerifyStatus<void>
verify_auth_claims_internal(const SignedTransaction &stx,
                            const AddressAuthData *source_auth_data)
{
  if constexpr (VerfType == VerificationType::kLightweightClaimedOnly) {
    assert(!source_auth_data);
  }

  const AuthClaim &auth_claim = stx.tx.auth_claim;

  const std::span<const DecoratedSignature> sigs(stx.signatures.data(),
                                                stx.signatures.size());

  const AddressAuthData *auth_data = nullptr;
  if constexpr (verify_weights_against_auth_data(VerfType)) {
    switch (auth_claim.claim_type()) {
    case AuthClaimType::kSourceAddressAuthData:
      if (source_auth_data == nullptr) {
        trace("Auth data mismatch: source address claim on an address with "
              "no auth data");
        return AuthVerifyError{AuthVerifyErrorCode::kAuthDataMismatch};
      }
      auth_data = source_auth_data;
      break;
    case AuthClaimType::kPubkeySignature:
      // Pubkey claims are only for accounts with no registered auth data.
      if (source_auth_data != nullptr) {
        trace("Auth data mismatch: pubkey claim on an address with "
              "registered auth data");
        return AuthVerifyError{AuthVerifyErrorCode::kAuthDataMismatch};
      }
      break;
    default:
      panic("Unsupported auth claim type: {}", auth_claim.claim_type());
    }
  }

  if (auto r = verify_auth_claim_internal<VerfType>(
          auth_claim, stx.pubkey_claim_key.get(),
          stx.tx.invocation.invoked_address, sigs, auth_data);
      !r) {
    return r;
  }

  if constexpr (verify_weights_against_auth_data(VerfType)) {
    if (auth_claim.claim_type() != AuthClaimType::kSourceAddressAuthData) {
      return {};
    }

    // No dedup needed: verify_weight_key_claim_internal already enforces
    // strictly-increasing public_key_id, so duplicates are impossible here.
    uint64_t total_weight = 0;
    for (const DecoratedSignature &sig : stx.signatures) {
      assert(has_public_key(*source_auth_data, sig.public_key_id));
      total_weight +=
          source_auth_data->public_keys[sig.public_key_id]->max_weight;
    }

    Weight required =
        std::max(source_auth_data->required_weight,
                 auth_claim.source_address_claim().required_weight);
    if (total_weight < required) {
      return AuthVerifyError{AuthVerifyErrorCode::kAuthDataMismatch};
    }
  }
  return {};
}

} // namespace

AuthVerifyStatus<void>
verify_auth(const SignedTransaction &stx,
            const AddressAuthData *source_auth_data,
            const NetworkHash &network_hash,
            Ed25519SignatureVerifier *signature_verifier)
{
  // Reject malformed claims, missing keys, and insufficient weight before
  // serializing the transaction or verifying/queuing any signatures.
  if (auto r = lightweight_validate_auth_claims(stx, source_auth_data); !r) {
    return r;
  }
  if (stx.signatures.empty()) {
    // The checks above allow this only for a zero-threshold source claim.
    return {};
  }

  const x23::Buf signature_preimage =
      transaction_signature_preimage(network_hash, stx.tx);
  if (stx.tx.auth_claim.claim_type() == AuthClaimType::kPubkeySignature) {
    return verify_signature(*stx.pubkey_claim_key,
                            stx.signatures.front().signature,
                            signature_preimage, signature_verifier);
  }
  // Check every supplied signature, even after the required weight is met.
  assert(source_auth_data);
  for (const DecoratedSignature &sig : stx.signatures) {
    if (auto r = verify_decorated_signature(sig, *source_auth_data,
                                            signature_preimage,
                                            signature_verifier);
        !r) {
      return r;
    }
  }
  return {};
}

AuthVerifyStatus<void>
lightweight_validate_auth_claims(const SignedTransaction &stx,
                                 const AddressAuthData *source_auth_data)
{
  return verify_auth_claims_internal<VerificationType::kLightweightClaimsValid>(
      stx, source_auth_data);
}

AuthVerifyStatus<void>
auth_claim_is_well_formed(const SignedTransaction &stx)
{
  return verify_auth_claims_internal<VerificationType::kLightweightClaimedOnly>(
      stx, nullptr);
}

} // namespace hogd
