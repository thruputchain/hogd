#include "verify.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <unordered_map>
#include <vector>

#include "address_auth.hh"
#include "auth_claim.hh"
#include "hogd/auth_claim/status.h"
#include "hogd/base/log.h"
#include "hogd/base/perthread.h"
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
  return ed25519_verify_batch(public_keys_, signatures_, serialized_txs_);
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
  return (bitmap & (0xFFFF'FFFF << (kMaxSigners))) == 0;
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
verify_individual_key_claim_internal(
    uint32_t key_id, const AddressAuthData *auth_data,
    x23::Vec<const DecoratedSignature *> const &sigs,
    x23::BufView serialized_tx, Ed25519SignatureVerifier *signature_verifier)
{
  if (sigs.size() != 1) {
    trace("Auth malformed: expected 1 signature, got {}", sigs.size());
    return AuthVerifyError{AuthVerifyErrorCode::kAuthMalformed};
  }
  if (sigs[0]->public_key_id != key_id) {
    trace("Auth malformed: signature id mismatch: expected {}, got {}", key_id,
          sigs[0]->public_key_id);
    return AuthVerifyError{AuthVerifyErrorCode::kAuthMalformed};
  }

  if constexpr (verify_weights_against_auth_data(VerfType)) {
    assert(auth_data);
    if (!has_public_key(*auth_data, key_id)) {
      trace("Auth malformed: public key {} not found in auth data", key_id);
      return AuthVerifyError{AuthVerifyErrorCode::kAuthDataMismatch};
    }
  }

  if constexpr (verify_signatures(VerfType)) {
    assert(auth_data);
    return verify_decorated_signature(*sigs[0], *auth_data, serialized_tx,
                                      signature_verifier);
  }
  return {};
}

template<VerificationType VerfType>
AuthVerifyStatus<void>
verify_weight_key_claim_internal(
    BitMap accepted_keys, Weight required_weight,
    const AddressAuthData *auth_data,
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
      trace("Auth malformed: weighted-claim signer ids must be strictly increasing");
      return AuthVerifyError{AuthVerifyErrorCode::kAuthMalformed};
    }
    prev_key_id = sig->public_key_id;
    have_prev_key_id = true;
  }

  if constexpr (verify_weights_against_auth_data(VerfType)) {
    assert(auth_data);
    size_t w = 0;
    for (const DecoratedSignature *sig : sigs) {
      if (!has_public_key(*auth_data, sig->public_key_id)) {
        return AuthVerifyError{AuthVerifyErrorCode::kAuthDataMismatch};
      }
      w += auth_data->public_keys[sig->public_key_id]->max_weight;
    }
    if (w < required_weight) {
      return AuthVerifyError{AuthVerifyErrorCode::kAuthDataMismatch};
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

template<VerificationType VerfType>
AuthVerifyStatus<void>
verify_auth_claim_internal(AuthClaim const &claim,
                           x23::Vec<DecoratedSignature const *> const &sigs,
                           const AddressAuthData *auth_data,
                           x23::BufView serialized_tx,
                           Ed25519SignatureVerifier *signature_verifier)
{
  if constexpr (VerfType == VerificationType::kLightweightClaimedOnly) {
    assert(!auth_data);
  }
  else {
    assert(auth_data);
  }

  switch (claim.claim_type()) {
  case AuthClaimType::kInvokedAddressSignature:
    return verify_individual_key_claim_internal<VerfType>(
        claim.invoked_address_signature_id(), auth_data, sigs, serialized_tx,
        signature_verifier);
  case AuthClaimType::kInvokedAddressSignedWeight:
    return verify_weight_key_claim_internal<VerfType>(
        claim.invoked_address_signed_weight().accepted_keys,
        claim.invoked_address_signed_weight().required_weight, auth_data, sigs,
        serialized_tx, signature_verifier);
  case AuthClaimType::kOtherAddressSignature:
    return verify_individual_key_claim_internal<VerfType>(
        claim.other_address_signature_id().key_id, auth_data, sigs,
        serialized_tx, signature_verifier);
  case AuthClaimType::kOtherAddressSignedWeight:
    return verify_weight_key_claim_internal<VerfType>(
        claim.other_address_signed_weight().accepted_keys,
        claim.other_address_signed_weight().required_weight, auth_data, sigs,
        serialized_tx, signature_verifier);
  default:
    // This kind of claim should not have been parseable
    panic("Unsupported auth claim type: {}", claim.claim_type());
  }
}

// This assumes that verify_auth_claim_internal has already been called and
// passed for each auth claim.
template<VerificationType VerfType>
requires (verify_weights_against_auth_data(VerfType))
AuthVerifyStatus<void>
validate_nonce(const SignedTransaction &stx, const AddressAuthData &auth_data,
               const NonceConfig &nonce_config)
{
  assert(stx.tx.auth_claims.size() == stx.signatures.size());
  Weight total_weight = 0;

  for (size_t i = 0; i < stx.signatures.size(); i++) {
    const DecoratedSignature &sig = stx.signatures[i];
    if (sig.auth_claim_idx >= stx.tx.auth_claims.size()) {
      continue;
    }
    const AuthClaim &auth_claim = stx.tx.auth_claims[sig.auth_claim_idx];
    switch (auth_claim.claim_type()) {
    case AuthClaimType::kInvokedAddressSignature:
    case AuthClaimType::kInvokedAddressSignedWeight:
      if (!bitmap_accepts_key(nonce_config.accepted_keys, sig.public_key_id)) {
        return AuthVerifyError{AuthVerifyErrorCode::kAuthDataMismatch};
      }
      total_weight += auth_data.public_keys[sig.public_key_id]->max_weight;
      break;
    case AuthClaimType::kOtherAddressSignature:
    case AuthClaimType::kOtherAddressSignedWeight:
      // other addresses can't contribute to the nonce weight
      break;
    default:
      panic("Unsupported auth claim type: {}", auth_claim.claim_type());
    }
  }
  return total_weight >= nonce_config.required_weight
             ? AuthVerifyStatus<void>{}
             : AuthVerifyError{AuthVerifyErrorCode::kAuthDataMismatch};
}

// gas_config is the gas permissions for the account mentioned in gas_claim
template<VerificationType VerfType>
requires (verify_weights_against_auth_data(VerfType))
AuthVerifyStatus<void>
validate_gas_permission(const SignedTransaction &stx, const GasClaim &gas_claim,
                        const AddressAuthData &auth_data)
{
  assert(stx.tx.auth_claims.size() == stx.signatures.size());
  x23::Vec<Weight, kMaxGasConfigs> gas_weights(auth_data.gas_permissions.size(),
                                               0);

  for (size_t i = 0; i < stx.signatures.size(); i++) {
    const DecoratedSignature &sig = stx.signatures[i];
    if (sig.auth_claim_idx >= stx.tx.auth_claims.size()) {
      continue;
    }
    const AuthClaim &auth_claim = stx.tx.auth_claims[sig.auth_claim_idx];
    const ContractAddress &claim_addr = [&auth_claim, &stx]() {
      switch (auth_claim.claim_type()) {
      case AuthClaimType::kInvokedAddressSignature:
      case AuthClaimType::kInvokedAddressSignedWeight:
        return stx.tx.invocation.invoked_address;
      case AuthClaimType::kOtherAddressSignature:
        return auth_claim.other_address_signature_id().address;
      case AuthClaimType::kOtherAddressSignedWeight:
        return auth_claim.other_address_signed_weight().address;
      default:
        panic("Unsupported auth claim type: {}", auth_claim.claim_type());
      }
    }();
    if (claim_addr != gas_claim.sponsored_contract) {
      continue;
    }
    for (size_t i = 0; i < auth_data.gas_permissions.size(); i++) {
      if (!bitmap_accepts_key(auth_data.gas_permissions[i].accepted_keys,
                              sig.public_key_id)) {
        continue;
      }
      assert(has_public_key(auth_data, sig.public_key_id));
      gas_weights[i] += auth_data.public_keys[sig.public_key_id]->max_weight;
    }
  }
  GasAmount approved_amount = 0;
  for (size_t i = 0; i < auth_data.gas_permissions.size(); i++) {
    if (gas_weights[i] >= auth_data.gas_permissions[i].required_weight) {
      approved_amount = std::max<GasAmount>(approved_amount,
                                            auth_data.gas_permissions[i].limit);
    }
  }
  return approved_amount >= gas_claim.amount
             ? AuthVerifyStatus<void>{}
             : AuthVerifyError{AuthVerifyErrorCode::kAuthDataMismatch};
}

template<VerificationType VerfType>
AuthVerifyStatus<void>
verify_auth_claims_internal(
    const SignedTransaction &stx, const AddressAuthData *source_auth_data,
    std::unordered_map<ContractAddress, const AddressAuthData *> const
        &aux_auth_data,
    Ed25519SignatureVerifier *signature_verifier)
{
  if constexpr (VerfType == VerificationType::kLightweightClaimedOnly) {
    assert(!source_auth_data);
    assert(aux_auth_data.empty());
  }
  else {
    assert(source_auth_data);
  }

  if constexpr (VerfType != VerificationType::kSignaturesValid) {
    assert(signature_verifier == nullptr);
  }

  x23::Buf serialized_tx = x23::serialize(stx.tx);
  if (stx.tx.auth_claims.size() != stx.signatures.size()) {
    return AuthVerifyError(AuthVerifyErrorCode::kAuthMalformed);
  }

  // Verify that each claimed auth has a corresponding set of valid signatures.
  static PerThread<
      std::unordered_map<size_t, x23::Vec<const DecoratedSignature *>>>
      sig_map;
  sig_map->clear();
  for (const DecoratedSignature &sig : stx.signatures) {
    (*sig_map)[sig.auth_claim_idx].push_back(&sig);
  }
  for (size_t i = 0; i < stx.tx.auth_claims.size(); i++) {
    const AuthClaim &auth_claim = stx.tx.auth_claims[i];
    const x23::Vec<const DecoratedSignature *> &sigs = (*sig_map)[i];
    const AddressAuthData *auth_data = nullptr;

    if constexpr (verify_weights_against_auth_data(VerfType)) {
      switch (auth_claim.claim_type()) {
      case AuthClaimType::kInvokedAddressSignature:
      case AuthClaimType::kInvokedAddressSignedWeight:
        auth_data = source_auth_data;
        break;
      case AuthClaimType::kOtherAddressSignature:
        assert(aux_auth_data.contains(
            auth_claim.other_address_signature_id().address));
        auth_data =
            aux_auth_data.at(auth_claim.other_address_signature_id().address);
        break;
      case AuthClaimType::kOtherAddressSignedWeight:
        assert(aux_auth_data.contains(
            auth_claim.other_address_signed_weight().address));
        auth_data =
            aux_auth_data.at(auth_claim.other_address_signed_weight().address);
        break;
      default:
        panic("Unsupported auth claim type: {}", auth_claim.claim_type());
      }
    }
    if (auto r = verify_auth_claim_internal<VerfType>(auth_claim, sigs,
                                                      auth_data, serialized_tx,
                                                      signature_verifier);
        !r) {
      return r;
    }
  }

  // The gas claims are only about gas sponsorships; the gas claim
  // from the source account should be implicit.
  for (const GasClaim &gas_claim : stx.tx.gas_claims) {
    if (gas_claim.sponsored_contract == stx.tx.invocation.invoked_address) {
      trace("Auth malformed: gas_claim.sponsored_contract == "
            "stx.tx.invocation.invoked_address");
      return AuthVerifyError(AuthVerifyErrorCode::kAuthMalformed);
    }
  }

  if constexpr (verify_weights_against_auth_data(VerfType)) {
    assert(source_auth_data);
    // Validate that the source account has enough weight to authorize the
    // nonce.
    if (auto r = validate_nonce<VerfType>(stx, *source_auth_data,
                                          source_auth_data->nonce_config);
        !r) {
      return r;
    }
    // Check all of the sponsored gas claims.
    for (const GasClaim &gas_claim : stx.tx.gas_claims) {
      assert(aux_auth_data.contains(gas_claim.sponsored_contract));
      if (auto r = validate_gas_permission<VerfType>(
              stx, gas_claim, *aux_auth_data.at(gas_claim.sponsored_contract));
          !r) {
        return r;
      }
    }

    GasClaim implicit_gas_claim =
        calculate_source_address_implicit_gas_claim(stx.tx);

    // Check the implicit gas sponsorship from the source account.
    if (implicit_gas_claim.amount > 0) {
      assert(source_auth_data);
      if (auto r = validate_gas_permission<VerfType>(stx, implicit_gas_claim,
                                                     *source_auth_data);
          !r) {
        return r;
      }
    }
  }
  return {};
}

} // namespace

AuthVerifyStatus<void>
verify_auth(const SignedTransaction &stx,
            const AddressAuthData &source_auth_data,
            std::unordered_map<ContractAddress, const AddressAuthData *> const
                &aux_auth_data,
            Ed25519SignatureVerifier *signature_verifier)
{
  return verify_auth_claims_internal<VerificationType::kSignaturesValid>(
      stx, &source_auth_data, aux_auth_data, signature_verifier);
}

AuthVerifyStatus<void>
lightweight_validate_auth_claims(
    const SignedTransaction &stx, const AddressAuthData &source_auth_data,
    std::unordered_map<ContractAddress, const AddressAuthData *> const
        &aux_auth_data)
{
  return verify_auth_claims_internal<VerificationType::kLightweightClaimsValid>(
      stx, &source_auth_data, aux_auth_data, nullptr);
}

AuthVerifyStatus<void>
auth_claim_is_well_formed(const SignedTransaction &stx)
{
  return verify_auth_claims_internal<VerificationType::kLightweightClaimedOnly>(
      stx, nullptr,
      std::unordered_map<ContractAddress, const AddressAuthData *>{}, nullptr);
}

} // namespace hogd
