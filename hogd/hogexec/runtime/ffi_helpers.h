#pragma once

#include <optional>
#include <type_traits>

#include "address_auth.hh"
#include "auth_claim.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/rust/contract_types/bindings.h"
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

template<> struct FFIHelperType<generated::AuthClaim> {
  static std::optional<AuthClaim>
  to_cpp(generated::AuthClaim const &ffi_auth_claim)
  {
    AuthClaim auth_claim{};

    switch (ffi_auth_claim.tag) {
    case generated::AuthClaim::AuthClaim_kSourceAddressSignedWeight:
      auth_claim.claim_type(AuthClaimType::kSourceAddressSignedWeight);
      auth_claim.accepted_keys() =
          ffi_auth_claim.k_source_address_signed_weight.accepted_keys;
      return auth_claim;
    case generated::AuthClaim::AuthClaim_kPubkeySignature:
      auth_claim.claim_type(AuthClaimType::kPubkeySignature);
      auth_claim.pubkey_preimage() =
          ffi_auth_claim.k_pubkey_signature.pubkey_preimage;
      return auth_claim;
    }
    error("Unknown auth claim type: {}",
          static_cast<int32_t>(ffi_auth_claim.tag));
    return std::nullopt;
  }

  static generated::AuthClaim to_ffi(AuthClaim const &cpp_auth_claim)
  {
    generated::AuthClaim ffi_auth_claim{};
    switch (cpp_auth_claim.claim_type()) {
    case AuthClaimType::kSourceAddressSignedWeight:
      ffi_auth_claim.k_source_address_signed_weight = {
          .tag = generated::AuthClaim::AuthClaim_kSourceAddressSignedWeight,
          .accepted_keys = cpp_auth_claim.accepted_keys(),
      };
      return ffi_auth_claim;
    case AuthClaimType::kPubkeySignature:
      panic("Unimplemented auth claim type: {}", cpp_auth_claim.claim_type());
    }
    panic("Unknown auth claim type: {}", cpp_auth_claim.claim_type());
  }

  constexpr static bool kFFIHelperRequired = true;
};

template<typename T>
concept is_ffi_helper_required = FFIHelperType<T>::kFFIHelperRequired;

static_assert(is_ffi_helper_required<generated::AuthClaim>);

static_assert(!is_ffi_helper_required<int64_t>);
} // namespace hogd
