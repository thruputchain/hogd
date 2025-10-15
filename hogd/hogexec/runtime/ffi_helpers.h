#pragma once

#include <type_traits>

#include "auth_claim.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/rust/contract_types/bindings.h"
#include "types.hh"

namespace hogd {

// Convert between generated FFI types and C++ types

static_assert(std::is_trivially_copyable_v<generated::AuthClaim>);
static_assert(std::is_trivially_copyable_v<ContractAddress>);

template<typename T> struct FFIHelperType;

template<> struct FFIHelperType<generated::AuthClaim> {
  static AuthClaim to_cpp(generated::AuthClaim const &ffi_auth_claim)
  {
    AuthClaim auth_claim;

    switch (ffi_auth_claim.tag) {
    case generated::AuthClaim::AuthClaim_kInvokedAddressSignature:
      auth_claim.claim_type(AuthClaimType::kInvokedAddressSignature);
      auth_claim.invoked_address_signature_id() =
          ffi_auth_claim.k_invoked_address_signature
              .invoked_address_signature_id;
      break;
    case generated::AuthClaim::AuthClaim_kInvokedAddressSignedWeight:
      auth_claim.claim_type(AuthClaimType::kInvokedAddressSignedWeight);
      auth_claim.invoked_address_signed_weight().accepted_keys =
          ffi_auth_claim.k_invoked_address_signed_weight
              .invoked_address_signed_weight.accepted_keys;
      auth_claim.invoked_address_signed_weight().required_weight =
          ffi_auth_claim.k_invoked_address_signed_weight
              .invoked_address_signed_weight.required_weight;
      break;
    case generated::AuthClaim::AuthClaim_kOtherAddressSignature:
      auth_claim.claim_type(AuthClaimType::kOtherAddressSignature);
      auth_claim.other_address_signature_id().address =
          ffi_auth_claim.k_other_address_signature.other_address_signature_id
              .address;
      auth_claim.other_address_signature_id().key_id =
          ffi_auth_claim.k_other_address_signature.other_address_signature_id
              .key_id;
      break;
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
      break;
    default:
      panic("Unknown auth claim type: {}",
            static_cast<int32_t>(ffi_auth_claim.tag));
    }

    return auth_claim;
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
