# Auth Claim Simplification Design

## Summary

Replace the 5-variant auth claim system with a single weighted-bitmap claim type. The new auth token system handles runtime authorization; auth claims now serve only one purpose: letting the scheduler/auth server determine whether a transaction has enough signing weight to start execution.

Each account stores a `required_weight` threshold. A transaction carries a single `kSourceAddressSignedWeight` claim with a bitmap of which keys signed. Verification sums the per-key weights from the account's auth data and checks against the threshold.

Gas sponsorship (`GasClaim`, `GasPermissionConfig`) and cross-contract auth claims (`kOtherAddress*`) are removed entirely. Gas sponsorship will be re-added later via wrapping envelopes.

## XDR Changes

### `auth_claim.x`

**Remove:**
- `kInvokedAddressSignature` (single-key invoked address claim)
- `kInvokedAddressSignedWeight` (weighted invoked address claim)
- `kOtherAddressSignature` (single-key cross-contract claim)
- `kOtherAddressSignedWeight` (weighted cross-contract claim)
- `kMaxGasConfigs` constant

**New `AuthClaimType` enum:**
```
enum AuthClaimType {
  kSourceAddressSignedWeight = 0,
  kPubkeySignature = 1
};
```

**New `AuthClaim` union:**
```
union AuthClaim switch (AuthClaimType claim_type) {
case kSourceAddressSignedWeight:
  BitMap accepted_keys;
case kPubkeySignature:
  PubkeyAccountPreimage pubkey_preimage;
};
```

The `kSourceAddressSignedWeight` variant carries only a `BitMap` — no `required_weight`. The threshold comes from the account's `AddressAuthData.required_weight`.

`kPubkeySignature` is kept in the enum but unimplemented (verification returns `kAuthMalformed`).

`kMaxAuthClaims`, `kMaxSigners`, `Weight`, `BitMap`, `KeyID`, `PubkeyAccountPreimage`, `AuthVerifyErrorCode` — all unchanged.

### `address_auth.x`

**Remove:**
- `GasPermissionConfig` struct
- `NonceConfig` struct

**New `AddressAuthData`:**
```
struct AddressAuthData {
  PublicKeyConfigOpt public_keys[kMaxSigners];
  Weight required_weight;
  BlockHeight last_written_height;
};
```

`PublicKeyConfig` (key + `max_weight`) is unchanged. `PublicKeyConfigOpt` is unchanged.

Default initialization constants unchanged: `kDefaultNewAddressRequiredWeight = 1`, `kDefaultNewAddressAcceptedKeys = 0xFFFFFFFF`, `kDefaultNewAddressKeyID = 0`.

### `transaction.x`

**Remove:**
- `GasClaim` struct
- `gas_claims` field from `Transaction`

The source address implicitly covers all gas. `calculate_source_address_implicit_gas_claim` is removed (gas_limit on the invocation is the total budget, paid by source).

## Verification Pipeline Changes

### `verify.h` / `verify.cc`

The three-step structure is preserved. Internal logic simplifies.

**Signature changes to public API:**
- `lightweight_validate_auth_claims` and `verify_auth`: remove the `aux_auth_data` map parameter. Only `const AddressAuthData& source_auth_data` is needed.
- `get_aux_addresses`: removed entirely.
- `calculate_source_address_implicit_gas_claim`: removed entirely.

**Well-formedness (`auth_claim_is_well_formed`):**
- For `kSourceAddressSignedWeight`: validate bitmap (no bits beyond `kMaxSigners`), validate each signature's `public_key_id` is in the bitmap, enforce strictly-increasing key IDs in signatures.
- For `kPubkeySignature`: return `kAuthMalformed` (unimplemented).

**Lightweight validation (`lightweight_validate_auth_claims`):**
- All well-formedness checks, plus: for each signature, confirm key exists in source `AddressAuthData`, sum `max_weight` values, check sum >= `auth_data.required_weight`.

**Full verification (`verify_auth`):**
- Same as lightweight plus Ed25519 batch signature verification via `Ed25519SignatureVerifier`.

**Removed internal functions:**
- `validate_nonce` — replaced by the single required_weight check in lightweight validation.
- `validate_gas_permission` — gas sponsorship deferred.
- `verify_individual_key_claim_internal` — no single-key claim type.
- All `kOtherAddress*` branches in `verify_auth_claim_internal`.

### Weight accumulation

The weight check now happens once per transaction (not per-claim + per-nonce + per-gas-config). For a transaction with one `kSourceAddressSignedWeight` claim:

1. Group signatures by `auth_claim_idx` (as today).
2. For each signature in the claim's group: confirm `public_key_id` is in the claim's `accepted_keys` bitmap and exists in `source_auth_data.public_keys`.
3. Sum `max_weight` for each unique key.
4. Check sum >= `source_auth_data.required_weight`.

## Downstream Changes

### Auth server / auth data cache

- The auth server only needs to fetch `AddressAuthData` for the source address per transaction. No auxiliary address lookups.
- `AuthDataCache` is unchanged in structure, just used for fewer lookups.
- RPC calls (`authdb_get_auth_data`, `authdb_get_auth_data_batch`) unchanged in interface, simplified in usage.

### Contract execution

- Contracts that read auth claims at runtime see `kSourceAddressSignedWeight` instead of the old types. This is a breaking change for any contract pattern-matching on claim types, but expected given the auth token migration.
- Any host functions exposing auth claim types to contracts need to handle the new enum values.

### Tests

- All auth claim verification tests rewritten for the new claim type.
- Gas permission tests removed.
- Nonce validation tests removed or reworked into required_weight threshold tests.
- Cross-contract auth tests removed.

## Not in scope

- `kPubkeySignature` implementation — placeholder only.
- Gas sponsorship via wrapping envelopes — future work.
- Removing `last_written_height` from `AddressAuthData` — tracked by existing TODO.
- Changes to `AuthTokenRegistry` or the auth token system — orthogonal.
