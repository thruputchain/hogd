#include "verify.h"

#include <algorithm>
#include <cassert>
#include <unordered_map>
#include <print>

#include "account.hh"
#include "auth_claim.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/crypto/crypto.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

namespace {

constexpr bool
valid_pk_claim(size_t idx)
{
  return idx < MAX_SIGNERS;
}

constexpr bool
valid_pk_bitmap(BitMap bitmap)
{
  // no pk has an index higher than MAX_SIGNERS
  static_assert(MAX_SIGNERS <= 32);
  return (bitmap & (0xFFFF'FFFF << (MAX_SIGNERS))) == 0;
}

constexpr bool
bitmap_accepts_key(BitMap bitmap, size_t idx)
{
  return (bitmap & (1 << idx)) != 0;
}

static_assert(valid_pk_claim(0));
static_assert(valid_pk_claim(19));
static_assert(!valid_pk_claim(20));

static_assert(valid_pk_bitmap(0));
static_assert(!valid_pk_bitmap(0xFFFF'FFFF));
static_assert(!valid_pk_bitmap(1 << MAX_SIGNERS));
static_assert(valid_pk_bitmap(1 << (MAX_SIGNERS - 1)));

static_assert(!bitmap_accepts_key(0, 0));
static_assert(bitmap_accepts_key(0xF, 2));

bool
has_public_key(const AccountAuthData &auth_data, size_t idx)
{
  return idx < auth_data.public_keys.size() && auth_data.public_keys[idx];
}

bool
verify_decorated_signature(const DecoratedSignature &dec_sig,
                           const AccountAuthData &auth_data,
                           SHA256Hash const &tx_hash)
{
  assert(has_public_key(auth_data, dec_sig.public_key_id));

  auto const &pk = auth_data.public_keys[dec_sig.public_key_id];
  assert(pk);

  std::println("tx hash {}", x23::show(tx_hash));

  switch (pk->key.scheme()) {
  case CryptographicScheme::Ed25519:
	{
    auto r = ed25519_verify(pk->key.ed25519(), dec_sig.signature.ed25519(),
                          tx_hash);
	std::println("verify sig result: {}", r);
	return r;
	}
		default:
    warn("Unsupported cryptographic scheme: {}", pk->key.scheme());
    return false;
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
bool
verify_individual_key_claim_internal(uint32_t key_id,
                                     const AccountAuthData *auth_data,
                                     DecoratedSignatureList const &sigs,
                                     SHA256Hash const &tx_hash)
{
  if (sigs.size() != 1) {
	  std::println("wrong keys");
    return false;
  }
  if (sigs[0].public_key_id != key_id) {
	  std::println("mismatch");
    return false;
  }

  if constexpr (verify_weights_against_auth_data(VerfType)) {
    assert(auth_data);
    if (!has_public_key(*auth_data, key_id)) {
	    std::println("no pk");
      return false;
    }
  }

  if constexpr (verify_signatures(VerfType)) {
    assert(auth_data);
    std::println("checking key");
    return verify_decorated_signature(sigs[0], *auth_data, tx_hash);
  }
  return true;
}

template<VerificationType VerfType>
bool
verify_weight_key_claim_internal(BitMap accepted_keys, Weight required_weight,
                                 const AccountAuthData *auth_data,
                                 DecoratedSignatureList const &sigs,
                                 SHA256Hash const &tx_hash)
{
  if (!valid_pk_bitmap(accepted_keys)) {
    return false;
  }

  for (const auto &sig : sigs) {
    if (!bitmap_accepts_key(accepted_keys, sig.public_key_id)) {
      return false;
    }
  }

  if constexpr (verify_weights_against_auth_data(VerfType)) {
    assert(auth_data);
    size_t w = 0;
    for (const auto &sig : sigs) {
      if (!has_public_key(*auth_data, sig.public_key_id)) {
        return false;
      }
      w += auth_data->public_keys[sig.public_key_id]->max_weight;
    }
    if (w < required_weight) {
      return false;
    }
  }

  if constexpr (verify_signatures(VerfType)) {
    assert(auth_data);
    return std::ranges::all_of(sigs, [&](const auto &sig) {
      return verify_decorated_signature(sig, *auth_data, tx_hash);
    });
  }
  return true;
}

template<VerificationType VerfType>
bool
verify_auth_claim_internal(AuthClaim const &claim,
                           DecoratedSignatureList const &sigs,
                           const AccountAuthData *auth_data,
                           SHA256Hash const &tx_hash)
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
        claim.invoked_address_signature_id(), auth_data, sigs, tx_hash);
  case AuthClaimType::kInvokedAddressSignedWeight:
    return verify_weight_key_claim_internal<VerfType>(
        claim.invoked_address_signed_weight().accepted_keys,
        claim.invoked_address_signed_weight().required_weight, auth_data, sigs,
        tx_hash);
  case AuthClaimType::kOtherAddressSignature:
    return verify_individual_key_claim_internal<VerfType>(
        claim.other_address_signature_id().key_id, auth_data, sigs, tx_hash);
  case AuthClaimType::kOtherAddressSignedWeight:
    return verify_weight_key_claim_internal<VerfType>(
        claim.other_address_signed_weight().accepted_keys,
        claim.other_address_signed_weight().required_weight, auth_data, sigs,
        tx_hash);
  default:
    // This kind of claim should not have been parseable
    panic("Unsupported auth claim type: {}", claim.claim_type());
  }
}

// This assumes that verify_auth_claim_internal has already been called and
// passed for each auth claim.
template<VerificationType VerfType>
requires (verify_weights_against_auth_data(VerfType))
bool
validate_nonce(const SignedTransaction &stx, const AccountAuthData &auth_data,
               const NonceConfig &nonce_config)
{
  assert(stx.tx.auth_claims.size() == stx.signatures.size());
  Weight total_weight = 0;

  for (size_t i = 0; i < stx.signatures.size(); i++) {
    const auto &sigs = stx.signatures[i];
    const auto &auth_claim = stx.tx.auth_claims[i];
    switch (auth_claim.claim_type()) {
    case AuthClaimType::kInvokedAddressSignature:
    case AuthClaimType::kInvokedAddressSignedWeight:
      for (const auto &sig : sigs) {
        if (!bitmap_accepts_key(nonce_config.accepted_keys,
                                sig.public_key_id)) {
          return false;
        }
        total_weight += auth_data.public_keys[sig.public_key_id]->max_weight;
      }
      break;
    case AuthClaimType::kOtherAddressSignature:
    case AuthClaimType::kOtherAddressSignedWeight:
      // other addresses can't contribute to the nonce weight
      break;
    default:
      panic("Unsupported auth claim type: {}", auth_claim.claim_type());
    }
  }
  return total_weight >= nonce_config.required_weight;
}

// gas_config is the gas permissions for the account mentioned in gas_claim
template<VerificationType VerfType>
requires (verify_weights_against_auth_data(VerfType))
bool
validate_gas_permission(const SignedTransaction &stx, const GasClaim &gas_claim,
                        const AccountAuthData &auth_data)
{
  assert(stx.tx.auth_claims.size() == stx.signatures.size());
  x23::Vec<Weight, MAX_GAS_CONFIGS> gas_weights(
      auth_data.gas_permissions.size(), 0);

  for (size_t i = 0; i < stx.signatures.size(); i++) {
    const auto &sigs = stx.signatures[i];
    const auto &auth_claim = stx.tx.auth_claims[i];

    const auto &claim_addr = [&auth_claim, &stx]() {
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
    for (const auto &sig : sigs) {
      for (size_t i = 0; i < auth_data.gas_permissions.size(); i++) {
        if (!bitmap_accepts_key(auth_data.gas_permissions[i].accepted_keys,
                                sig.public_key_id)) {
          continue;
        }
        assert(has_public_key(auth_data, sig.public_key_id));
        gas_weights[i] += auth_data.public_keys[sig.public_key_id]->max_weight;
      }
    }
  }
  GasAmount approved_amount = 0;
  for (size_t i = 0; i < auth_data.gas_permissions.size(); i++) {
    if (gas_weights[i] >= auth_data.gas_permissions[i].required_weight) {
      approved_amount = std::max<GasAmount>(approved_amount,
                                            auth_data.gas_permissions[i].limit);
    }
  }
  return approved_amount >= gas_claim.amount;
}

template<VerificationType VerfType>
bool
verify_auth_claims_internal(
    const SignedTransaction &stx, const AccountAuthData *source_auth_data,
    std::unordered_map<ContractAddress, const AccountAuthData *> const
        &aux_auth_data)
{
  if constexpr (VerfType == VerificationType::kLightweightClaimedOnly) {
    assert(!source_auth_data);
    assert(aux_auth_data.empty());
  }
  else {
    assert(source_auth_data);
  }

  SHA256Hash tx_hash = sha256(stx.tx);
  if (stx.tx.auth_claims.size() != stx.signatures.size()) {
    return false;
  }

  for (size_t i = 0; i < stx.tx.auth_claims.size(); i++) {
    auto const &auth_claim = stx.tx.auth_claims[i];
    auto const &sigs = stx.signatures[i];
    const AccountAuthData *auth_data = nullptr;
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
    if (!verify_auth_claim_internal<VerfType>(auth_claim, sigs, auth_data,
                                              tx_hash)) {
	    std::println("failed to validate claim {} with sigs {} on auth_data {}", x23::show(auth_claim), x23::show(sigs), x23::show(*auth_data));
	return false;
    }
  }

  for (auto const &gas_claim : stx.tx.gas_claims) {
    if (gas_claim.sponsored_contract == stx.tx.invocation.invoked_address) {
      std::println("source gas claim");
      return false;
    }
  }

  if constexpr (verify_weights_against_auth_data(VerfType)) {
    assert(source_auth_data);
    if (!validate_nonce<VerfType>(stx, *source_auth_data,
                                  source_auth_data->nonce_config)) {
	    std::println("bad nonce");
	   return false;
    }
    GasAmount sponsored_gas = 0;
    for (const auto &gas_claim : stx.tx.gas_claims) {
      sponsored_gas += gas_claim.amount;
      assert(aux_auth_data.contains(gas_claim.sponsored_contract));
      if (!validate_gas_permission<VerfType>(
              stx, gas_claim,
              *aux_auth_data.at(gas_claim.sponsored_contract))) {
	      std::println("bad gas nonce");
	return false;
      }
    }

    if (sponsored_gas < stx.tx.invocation.gas_limit) {
      GasAmount remaining = stx.tx.invocation.gas_limit - sponsored_gas;
      assert(source_auth_data);
      if (!validate_gas_permission<VerfType>(
              stx,
              GasClaim{.sponsored_contract = stx.tx.invocation.invoked_address,
                       .amount = remaining},
              *source_auth_data)) {
        return false;
      }
    }
  }
  return true;
}

} // namespace

bool
verify_auth(const SignedTransaction &stx,
            const AccountAuthData &source_auth_data,
            std::unordered_map<ContractAddress, const AccountAuthData *> const
                &aux_auth_data)
{
	std::println("verifying {} against {}", x23::show(stx), x23::show(source_auth_data));
  return verify_auth_claims_internal<VerificationType::kSignaturesValid>(
      stx, &source_auth_data, aux_auth_data);
}

bool
lightweight_validate_auth_claims(
    const SignedTransaction &stx, const AccountAuthData &source_auth_data,
    std::unordered_map<ContractAddress, const AccountAuthData *> const
        &aux_auth_data)
{
  return verify_auth_claims_internal<VerificationType::kLightweightClaimsValid>(
      stx, &source_auth_data, aux_auth_data);
}

bool
lightweight_plausible_auth_claims(const SignedTransaction &stx)
{
  return verify_auth_claims_internal<VerificationType::kLightweightClaimedOnly>(
      stx, nullptr,
      std::unordered_map<ContractAddress, const AccountAuthData *>{});
}

} // namespace hogd
