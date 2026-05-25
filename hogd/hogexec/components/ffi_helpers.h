#pragma once

#include <cstring>
#include <optional>
#include <type_traits>

#include "auth_claim.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/components/ffi_types.h"
#include "types.hh"

namespace hogd {

// Convert between generated FFI types and C++ types
//
// Note that the generated FFI types may come from a contract,
// which are NOT TRUSTED.  Therefore, any possibly fallible method
// that converts to C++ types must be infallible.

static_assert(std::is_trivially_copyable_v<generated::AuthClaim>);
static_assert(std::is_trivially_copyable_v<ContractAddress>);

template<typename T> struct FFIHelperType;

template<> struct FFIHelperType<generated::PublicKey> {
  static std::optional<PublicKey>
  to_cpp(generated::PublicKey const &ffi_public_key)
  {
    switch (ffi_public_key.tag) {
    case generated::PublicKey::PublicKey_Ed25519:
      PublicKey cpp_public_key;
      cpp_public_key.scheme(CryptographicScheme::Ed25519);
      cpp_public_key.ed25519() = ffi_public_key.ed25519.ed25519;
      return cpp_public_key;
    }
    error("Unknown public key type: {}",
          static_cast<int32_t>(ffi_public_key.tag));
    return std::nullopt;
  }

  static generated::PublicKey to_ffi(PublicKey const &cpp_public_key)
  {
    generated::PublicKey ffi_public_key{};
    ffi_public_key.tag = generated::PublicKey::PublicKey_Ed25519;
    ffi_public_key.ed25519.ed25519 = cpp_public_key.ed25519();
    return ffi_public_key;
  }

  constexpr static bool kFFIHelperRequired = true;
};

static_assert(std::is_trivially_copyable_v<generated::PublicKey>);
static_assert(sizeof(generated::PublicKey) == sizeof(PublicKey));

template<> struct FFIHelperType<generated::PubkeyAccountPreimage> {
  static std::optional<PubkeyAccountPreimage>
  to_cpp(generated::PubkeyAccountPreimage const &ffi_pubkey_account_preimage)
  {
    PubkeyAccountPreimage cpp_pubkey_account_preimage;
    auto cpp_pk = FFIHelperType<generated::PublicKey>::to_cpp(
        ffi_pubkey_account_preimage.pubkey);
    if (!cpp_pk) {
      return std::nullopt;
    }
    cpp_pubkey_account_preimage.pubkey = *cpp_pk;
    cpp_pubkey_account_preimage.nonce = ffi_pubkey_account_preimage.nonce;
    return cpp_pubkey_account_preimage;
  }

  static generated::PubkeyAccountPreimage
  to_ffi(PubkeyAccountPreimage const &cpp_pubkey_account_preimage)
  {
    generated::PubkeyAccountPreimage ffi_pubkey_account_preimage{};
    ffi_pubkey_account_preimage.pubkey =
        FFIHelperType<generated::PublicKey>::to_ffi(
            cpp_pubkey_account_preimage.pubkey);
    ffi_pubkey_account_preimage.nonce = cpp_pubkey_account_preimage.nonce;
    return ffi_pubkey_account_preimage;
  }

  constexpr static bool kFFIHelperRequired = true;
};

static_assert(std::is_trivially_copyable_v<generated::PubkeyAccountPreimage>);
static_assert(sizeof(generated::PubkeyAccountPreimage) ==
              sizeof(PubkeyAccountPreimage));

template<> struct FFIHelperType<generated::AuthClaim> {
  static std::optional<AuthClaim>
  to_cpp(generated::AuthClaim const &ffi_auth_claim)
  {
    AuthClaim auth_claim{};

    switch (ffi_auth_claim.tag) {
    case generated::AuthClaim::AuthClaim_kSourceAddressAuthData:
      auth_claim.claim_type(AuthClaimType::kSourceAddressAuthData);
      auth_claim.source_address_claim().allowed_keys =
          ffi_auth_claim.k_source_address_auth_data.source_address_claim
              .allowed_keys;
      auth_claim.source_address_claim().required_weight =
          ffi_auth_claim.k_source_address_auth_data.source_address_claim
              .required_weight;
      return auth_claim;
    case generated::AuthClaim::AuthClaim_kPubkeySignature:
    {
      auth_claim.claim_type(AuthClaimType::kPubkeySignature);
      auto cpp_pubkey_preimage =
          FFIHelperType<generated::PubkeyAccountPreimage>::to_cpp(
              ffi_auth_claim.k_pubkey_signature.pubkey_preimage);
      if (!cpp_pubkey_preimage) {
        return std::nullopt;
      }
      auth_claim.pubkey_preimage() = *cpp_pubkey_preimage;
      return auth_claim;
    }
    }
    error("Unknown auth claim type: {}",
          static_cast<int32_t>(ffi_auth_claim.tag));
    return std::nullopt;
  }

  static generated::AuthClaim to_ffi(AuthClaim const &cpp_auth_claim)
  {
    generated::AuthClaim ffi_auth_claim{};
    switch (cpp_auth_claim.claim_type()) {
    case AuthClaimType::kSourceAddressAuthData:
      ffi_auth_claim.k_source_address_auth_data = {
          .tag = generated::AuthClaim::AuthClaim_kSourceAddressAuthData,
          .source_address_claim =
              {
                  .allowed_keys =
                      cpp_auth_claim.source_address_claim().allowed_keys,
                  .required_weight =
                      cpp_auth_claim.source_address_claim().required_weight,
              },
      };
      return ffi_auth_claim;
    case AuthClaimType::kPubkeySignature:
      ffi_auth_claim.k_pubkey_signature = {
          .tag = generated::AuthClaim::AuthClaim_kPubkeySignature,
          .pubkey_preimage =
              FFIHelperType<generated::PubkeyAccountPreimage>::to_ffi(
                  cpp_auth_claim.pubkey_preimage()),
      };
      return ffi_auth_claim;
    }
    panic("Unknown auth claim type: {}", cpp_auth_claim.claim_type());
  }

  constexpr static bool kFFIHelperRequired = true;
};

static_assert(std::is_trivially_copyable_v<generated::AuthClaim>);

template<typename T>
concept is_ffi_helper_required = FFIHelperType<T>::kFFIHelperRequired;

static_assert(is_ffi_helper_required<generated::AuthClaim>);

static_assert(!is_ffi_helper_required<int64_t>);
} // namespace hogd
