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
  static std::optional<AuthClaim> to_cpp(generated::AuthClaim const &ffi_auth_claim)
  {
    AuthClaim auth_claim;

    switch (ffi_auth_claim.tag) {
    case generated::AuthClaim::AuthClaim_kInvokedAddressSignature:
      auth_claim.claim_type(AuthClaimType::kInvokedAddressSignature);
      auth_claim.invoked_address_signature_id() =
          ffi_auth_claim.k_invoked_address_signature
              .invoked_address_signature_id;
      return auth_claim;
    case generated::AuthClaim::AuthClaim_kInvokedAddressSignedWeight:
      auth_claim.claim_type(AuthClaimType::kInvokedAddressSignedWeight);
      auth_claim.invoked_address_signed_weight().accepted_keys =
          ffi_auth_claim.k_invoked_address_signed_weight
              .invoked_address_signed_weight.accepted_keys;
      auth_claim.invoked_address_signed_weight().required_weight =
          ffi_auth_claim.k_invoked_address_signed_weight
              .invoked_address_signed_weight.required_weight;
      return auth_claim;
    case generated::AuthClaim::AuthClaim_kOtherAddressSignature:
      auth_claim.claim_type(AuthClaimType::kOtherAddressSignature);
      auth_claim.other_address_signature_id().address =
          ffi_auth_claim.k_other_address_signature.other_address_signature_id
              .address;
      auth_claim.other_address_signature_id().key_id =
          ffi_auth_claim.k_other_address_signature.other_address_signature_id
              .key_id;
      return auth_claim;
    case generated::AuthClaim::AuthClaim_kOtherAddressSignedWeight:
      auth_claim.claim_type(AuthClaimType::kOtherAddressSignedWeight);
      auth_claim.other_address_signed_weight() = {
          .address = ffi_auth_claim.k_other_address_signed_weight
                         .other_address_signed_weight.address,
          .accepted_keys = ffi_auth_claim.k_other_address_signed_weight
                               .other_address_signed_weight.accepted_keys,
          .required_weight = ffi_auth_claim.k_other_address_signed_weight
                                 .other_address_signed_weight.required_weight,
      };
      return auth_claim;
    // TODO(geoff): Implement PubkeySignature
    }
    error("Unknown auth claim type: {}", static_cast<int32_t>(ffi_auth_claim.tag));
    return std::nullopt;
  }

  static generated::AuthClaim to_ffi(AuthClaim const &cpp_auth_claim)
  {
    generated::AuthClaim ffi_auth_claim;
    switch (cpp_auth_claim.claim_type()) {
    case AuthClaimType::kInvokedAddressSignature:
      ffi_auth_claim.k_invoked_address_signature = {
          .tag = generated::AuthClaim::AuthClaim_kInvokedAddressSignature,
          .invoked_address_signature_id =
              cpp_auth_claim.invoked_address_signature_id(),
      };
      return ffi_auth_claim;
    case AuthClaimType::kInvokedAddressSignedWeight:
      ffi_auth_claim.k_invoked_address_signed_weight = {
          .tag = generated::AuthClaim::AuthClaim_kInvokedAddressSignedWeight,
          .invoked_address_signed_weight =
              {
                  .accepted_keys =
                      cpp_auth_claim.invoked_address_signed_weight()
                          .accepted_keys,
                  .required_weight =
                      cpp_auth_claim.invoked_address_signed_weight()
                          .required_weight,
              },
      };
      return ffi_auth_claim;
    case AuthClaimType::kOtherAddressSignature:
      ffi_auth_claim.k_other_address_signature = {
          .tag = generated::AuthClaim::AuthClaim_kOtherAddressSignature,
          .other_address_signature_id =
              {
                  .address =
                      cpp_auth_claim.other_address_signature_id().address,
                  .key_id = cpp_auth_claim.other_address_signature_id().key_id,
              },
      };
      return ffi_auth_claim;
    case AuthClaimType::kOtherAddressSignedWeight:
      ffi_auth_claim.k_other_address_signed_weight = {
          .tag = generated::AuthClaim::AuthClaim_kOtherAddressSignedWeight,
          .other_address_signed_weight =
              {
                  .address =
                      cpp_auth_claim.other_address_signed_weight().address,
                  .accepted_keys = cpp_auth_claim.other_address_signed_weight()
                                       .accepted_keys,
                  .required_weight =
                      cpp_auth_claim.other_address_signed_weight()
                          .required_weight,
              },
      };
      return ffi_auth_claim;
    case AuthClaimType::kPubkeySignature:
      panic("Unimplemented auth claim type: {}", cpp_auth_claim.claim_type());
    }
    panic("Unknown auth claim type: {}", cpp_auth_claim.claim_type());
  }

  constexpr static bool kFFIHelperRequired = true;
};

template<> struct FFIHelperType<generated::NonceConfig> {
  static NonceConfig to_cpp(generated::NonceConfig const &ffi_nonce_config)
  {
    return NonceConfig{
        .accepted_keys = ffi_nonce_config.accepted_keys,
        .required_weight = ffi_nonce_config.required_weight,
    };
  }

  static generated::NonceConfig to_ffi(NonceConfig const &cpp_nonce_config)
  {
    return generated::NonceConfig{
        .accepted_keys = cpp_nonce_config.accepted_keys,
        .required_weight = cpp_nonce_config.required_weight,
    };
  }
  constexpr static bool kFFIHelperRequired = true;
};

template<> struct FFIHelperType<generated::GasPermissionConfig> {
  static GasPermissionConfig
  to_cpp(generated::GasPermissionConfig const &ffi_gas_permission_config)
  {
    return GasPermissionConfig{
        .limit = ffi_gas_permission_config.limit,
        .accepted_keys = ffi_gas_permission_config.accepted_keys,
        .required_weight = ffi_gas_permission_config.required_weight,
    };
  }

  static generated::GasPermissionConfig
  to_ffi(GasPermissionConfig const &cpp_gas_permission_config)
  {
    return generated::GasPermissionConfig{
        .limit = cpp_gas_permission_config.limit,
        .accepted_keys = cpp_gas_permission_config.accepted_keys,
        .required_weight = cpp_gas_permission_config.required_weight,
    };
  }
  constexpr static bool kFFIHelperRequired = true;
};

template<typename T>
concept is_ffi_helper_required = FFIHelperType<T>::kFFIHelperRequired;

static_assert(is_ffi_helper_required<generated::AuthClaim>);
static_assert(is_ffi_helper_required<generated::NonceConfig>);
static_assert(is_ffi_helper_required<generated::GasPermissionConfig>);

static_assert(!is_ffi_helper_required<int64_t>);
} // namespace hogd
