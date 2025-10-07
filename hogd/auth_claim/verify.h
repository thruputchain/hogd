#pragma once

#include <unordered_map>

#include "account.hh"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "transaction.hh"
#include "types.hh"

namespace hogd {

// Lightweight verification that the signed transaction is well-formed.
// Does not validate any signatures.  Checks that
// each DecoratedSignature has a valid public key id, and that bitmaps are
// valid. Does not check against any items in the database.
bool lightweight_plausible_auth_claims(const SignedTransaction &stx);

// Performs all of the checks of lightweight_plausible_auth_claims,
// as well as checks that the signatures correspond to public keys that exist,
// and that claims about signature weights are satisfied.
bool lightweight_validate_auth_claims(
    const SignedTransaction &stx, const AccountAuthData &source_auth_data,
    std::unordered_map<ContractAddress, const AccountAuthData *> const
        &aux_auth_data);

// Verifies that the signatures on the transaction match the claims about the
// transaction in the auth claims field.
// Equivalent to lightweight_validate_auth_claims + the actual signature
// verification.
bool
verify_auth(const SignedTransaction &stx,
            const AccountAuthData &source_auth_data,
            std::unordered_map<ContractAddress, const AccountAuthData *> const
                &aux_auth_data);

} // namespace hogd
