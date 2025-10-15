#pragma once

#include <unordered_map>
#include <unordered_set>

#include "account.hh"
#include "auth_claim.hh"
#include "hogd/auth_claim/status.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "transaction.hh"
#include "types.hh"

namespace hogd {

// There are three steps to validating a transaction for scheduling:
// 1. Verification that the transaction is well-formed.  This requires
// a small number of checks beyond parsing the transaction data into the
// correct format.  These checks can be performed by reading only the
// transaction data.
// 2. Verification that the auth data referred to by the transaction exists.
// This requires loading all of the auth data referenced by the transaction
// from the database.
// 3. Verification that the signatures on the transaction are all valid,
// using the auth data loaded in step 2.
//
// These steps are increasinbly expensive computationally.  So we would like to
// not perform any of the work in later steps if the earlier ones fail.

// Lightweight verification that the signed transaction is well-formed.
// Does not validate any signatures.  Checks that
// each DecoratedSignature has a valid public key id, and that bitmaps are
// valid. Does not check against any items in the database.
AuthVerifyStatus<void> auth_claim_is_well_formed(const SignedTransaction &stx);

// Performs all of the checks of auth_claim_is_well_formed,
// as well as checks that the signatures correspond to public keys that exist,
// and that claims about signature weights are satisfied.
AuthVerifyStatus<void> lightweight_validate_auth_claims(
    const SignedTransaction &stx, const AccountAuthData &source_auth_data,
    std::unordered_map<ContractAddress, const AccountAuthData *> const
        &aux_auth_data);

// Verifies that the signatures on the transaction match the claims about the
// transaction in the auth claims field.
// Equivalent to lightweight_validate_auth_claims + the actual signature
// verification.
AuthVerifyStatus<void>
verify_auth(const SignedTransaction &stx,
            const AccountAuthData &source_auth_data,
            std::unordered_map<ContractAddress, const AccountAuthData *> const
                &aux_auth_data);

// Returns the implicit gas claim for the source address.
// This is calculated as the amount of gas that is not covered by sponsored
// gas claims attached to the transaction.
constexpr GasClaim
calculate_source_address_implicit_gas_claim(const Transaction &tx)
{
  GasAmount remaining = tx.invocation.gas_limit;
  for (const GasClaim &gas_claim : tx.gas_claims) {
    if (remaining > gas_claim.amount) {
      remaining -= gas_claim.amount;
    }
    else {
      remaining = 0;
      break;
    }
  }
  return GasClaim{.sponsored_contract = tx.invocation.invoked_address,
                  .amount = remaining};
}

inline std::unordered_set<ContractAddress>
get_aux_addresses(const Transaction &tx)
{
  std::unordered_set<ContractAddress> aux_addresses;
  for (const GasClaim &gas_claim : tx.gas_claims) {
    aux_addresses.insert(gas_claim.sponsored_contract);
  }
  for (const AuthClaim &auth_claim : tx.auth_claims) {
    switch (auth_claim.claim_type()) {
    case AuthClaimType::kOtherAddressSignature:
      aux_addresses.insert(auth_claim.other_address_signature_id().address);
      break;
    case AuthClaimType::kOtherAddressSignedWeight:
      aux_addresses.insert(auth_claim.other_address_signed_weight().address);
      break;
    default:
      break;
    }
  }
  return aux_addresses;
}

} // namespace hogd
