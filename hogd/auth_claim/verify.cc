#include "verify.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <span>
#include <vector>

#include "address_auth.hh"
#include "auth_claim.hh"
#include "hogd/auth_claim/status.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/crypto/crypto.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

void
Ed25519SignatureVerifier::add_signature(const Ed25519PublicKey &public_key,
                                        const Ed25519Signature &signature,
                                        x23::BufView serialized_tx)
{
  public_keys_.push_back(public_key);
  signatures_.push_back(signature);
  serialized_txs_.emplace_back(serialized_tx.begin(), serialized_tx.end());
}

bool
Ed25519SignatureVerifier::verify()
{
  return ed25519_verify_batch(public_keys_, signatures_,
                              std::span<const x23::Buf>(serialized_txs_));
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

AuthVerifyStatus<void>
verify_decorated_signature(const DecoratedSignature &dec_sig,
                           const AddressAuthData &auth_data,
                           x23::BufView serialized_tx,
                           Ed25519SignatureVerifier *signature_verifier)
{
  assert(has_public_key(auth_data, dec_sig.public_key_id));

  const PublicKeyConfigOpt &pk = auth_data.public_keys[dec_sig.public_key_id];
  assert(pk);

  switch (pk->key.scheme()) {
  case CryptographicScheme::Ed25519:
  {
    if (signature_verifier) {
      signature_verifier->add_signature(pk->key.ed25519(),
                                        dec_sig.signature.ed25519(),
                                        serialized_tx);
      return {};
    }
    auto r = ed25519_verify(pk->key.ed25519(), dec_sig.signature.ed25519(),
                            serialized_tx);
    if (!r) {
      trace("Signature verification failed");
    }
    return r ? AuthVerifyStatus<void>{}
             : AuthVerifyError{
                   AuthVerifyErrorCode::kSignatureVerificationFailed};
  }
  default:
    warn("Unsupported cryptographic scheme: {}", pk->key.scheme());
    return AuthVerifyError{AuthVerifyErrorCode::kAuthMalformed};
  }
}

enum class VerificationType {
  kLightweightClaimedOnly,
  kLightweightClaimsValid,
  kSignaturesValid,
};

constexpr bool
verify_weights_against_auth_data(VerificationType VerfType)
{
  return VerfType == VerificationType::kLightweightClaimsValid ||
         VerfType == VerificationType::kSignaturesValid;
}

constexpr bool
verify_signatures(VerificationType VerfType)
{
  return VerfType == VerificationType::kSignaturesValid;
}

template<VerificationType VerfType>
AuthVerifyStatus<void>
verify_weight_key_claim_internal(
    BitMap accepted_keys, const AddressAuthData *auth_data,
    const x23::Vec<const DecoratedSignature *> &sigs,
    x23::BufView serialized_tx, Ed25519SignatureVerifier *signature_verifier)
{
  if (!valid_pk_bitmap(accepted_keys)) {
    return AuthVerifyError{AuthVerifyErrorCode::kAuthMalformed};
  }

  uint32_t prev_key_id = 0;
  bool have_prev_key_id = false;
  for (const DecoratedSignature *sig : sigs) {
    if (!bitmap_accepts_key(accepted_keys, sig->public_key_id)) {
      return AuthVerifyError{AuthVerifyErrorCode::kAuthMalformed};
    }
    if (have_prev_key_id && sig->public_key_id <= prev_key_id) {
      trace("Auth malformed: weighted-claim signer ids must be strictly "
            "increasing");
      return AuthVerifyError{AuthVerifyErrorCode::kAuthMalformed};
    }
    prev_key_id = sig->public_key_id;
    have_prev_key_id = true;
  }

  if constexpr (verify_weights_against_auth_data(VerfType)) {
    assert(auth_data);
    for (const DecoratedSignature *sig : sigs) {
      if (!has_public_key(*auth_data, sig->public_key_id)) {
        return AuthVerifyError{AuthVerifyErrorCode::kAuthDataMismatch};
      }
    }
  }

  if constexpr (verify_signatures(VerfType)) {
    assert(auth_data);

    for (const DecoratedSignature *sig : sigs) {
      if (auto r = verify_decorated_signature(*sig, *auth_data, serialized_tx,
                                              signature_verifier);
          !r) {
        return r;
      }
    }
  }
  return {};
}

// Verifies a kPubkeySignature claim: a single signature from a standalone
// public key, for an account whose address is the hash of the pubkey
// preimage and which has no AddressAuthData registered onchain (that
// precondition is enforced by the caller, which has the DB data).
template<VerificationType VerfType>
AuthVerifyStatus<void>
verify_pubkey_claim_internal(const PubkeyAccountPreimage &preimage,
                             const ContractAddress &invoked_address,
                             const x23::Vec<const DecoratedSignature *> &sigs,
                             x23::BufView serialized_tx,
                             Ed25519SignatureVerifier *signature_verifier)
{
  if (sigs.size() != 1) {
    trace("Auth malformed: pubkey claim requires exactly one signature");
    return AuthVerifyError{AuthVerifyErrorCode::kAuthMalformed};
  }
  const DecoratedSignature &sig = *sigs[0];
  if (sig.public_key_id != kDefaultNewAddressKeyID) {
    trace("Auth malformed: pubkey claim signature must use key id {}",
          kDefaultNewAddressKeyID);
    return AuthVerifyError{AuthVerifyErrorCode::kAuthMalformed};
  }
  if (preimage.pubkey.scheme() != CryptographicScheme::Ed25519 ||
      sig.signature.scheme() != CryptographicScheme::Ed25519) {
    warn("Unsupported cryptographic scheme: {}", preimage.pubkey.scheme());
    return AuthVerifyError{AuthVerifyErrorCode::kAuthMalformed};
  }
  if (sha256(preimage) != invoked_address) {
    trace("Auth malformed: pubkey preimage hash does not match the invoked "
          "address");
    return AuthVerifyError{AuthVerifyErrorCode::kAuthMalformed};
  }

  if constexpr (verify_signatures(VerfType)) {
    if (signature_verifier) {
      signature_verifier->add_signature(preimage.pubkey.ed25519(),
                                        sig.signature.ed25519(), serialized_tx);
      return {};
    }
    if (!ed25519_verify(preimage.pubkey.ed25519(), sig.signature.ed25519(),
                        serialized_tx)) {
      trace("Signature verification failed");
      return AuthVerifyError{AuthVerifyErrorCode::kSignatureVerificationFailed};
    }
  }
  return {};
}

template<VerificationType VerfType>
AuthVerifyStatus<void>
verify_auth_claim_internal(AuthClaim const &claim,
                           ContractAddress const &invoked_address,
                           x23::Vec<DecoratedSignature const *> const &sigs,
                           const AddressAuthData *auth_data,
                           x23::BufView serialized_tx,
                           Ed25519SignatureVerifier *signature_verifier)
{
  switch (claim.claim_type()) {
  case AuthClaimType::kSourceAddressAuthData:
    if constexpr (VerfType == VerificationType::kLightweightClaimedOnly) {
      assert(!auth_data);
    }
    else {
      assert(auth_data);
    }
    return verify_weight_key_claim_internal<VerfType>(
        claim.source_address_claim().allowed_keys, auth_data, sigs,
        serialized_tx, signature_verifier);
  case AuthClaimType::kPubkeySignature:
    // The no-auth-entry precondition is checked by the caller (it owns the
    // DB data); everything here is derivable from the transaction alone.
    return verify_pubkey_claim_internal<VerfType>(claim.pubkey_preimage(),
                                                  invoked_address, sigs,
                                                  serialized_tx,
                                                  signature_verifier);
  default:
    // This kind of claim should not have been parseable
    panic("Unsupported auth claim type: {}", claim.claim_type());
  }
}

template<VerificationType VerfType>
AuthVerifyStatus<void>
verify_auth_claims_internal(const SignedTransaction &stx,
                            const AddressAuthData *source_auth_data,
                            Ed25519SignatureVerifier *signature_verifier)
{
  if constexpr (VerfType == VerificationType::kLightweightClaimedOnly) {
    assert(!source_auth_data);
  }

  if constexpr (VerfType != VerificationType::kSignaturesValid) {
    assert(signature_verifier == nullptr);
  }

  x23::Buf serialized_tx = x23::serialize(stx.tx);

  const AuthClaim &auth_claim = stx.tx.auth_claim;

  // Collect all signatures into a vec for the single claim.
  x23::Vec<const DecoratedSignature *> sigs;
  for (const DecoratedSignature &sig : stx.signatures) {
    sigs.push_back(&sig);
  }

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
          auth_claim, stx.tx.invocation.invoked_address, sigs, auth_data,
          serialized_tx, signature_verifier);
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
            Ed25519SignatureVerifier *signature_verifier)
{
  return verify_auth_claims_internal<VerificationType::kSignaturesValid>(
      stx, source_auth_data, signature_verifier);
}

AuthVerifyStatus<void>
lightweight_validate_auth_claims(const SignedTransaction &stx,
                                 const AddressAuthData *source_auth_data)
{
  return verify_auth_claims_internal<VerificationType::kLightweightClaimsValid>(
      stx, source_auth_data, nullptr);
}

AuthVerifyStatus<void>
auth_claim_is_well_formed(const SignedTransaction &stx)
{
  return verify_auth_claims_internal<VerificationType::kLightweightClaimedOnly>(
      stx, nullptr, nullptr);
}

} // namespace hogd
