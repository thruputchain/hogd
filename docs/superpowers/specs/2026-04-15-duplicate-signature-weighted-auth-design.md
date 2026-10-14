# Reject duplicate weighted-claim signers and dedupe aggregate auth weight

**Issue:** [#1442](https://github.com/groundhogchain/hogd/issues/1442)
**Date:** 2026-04-15

## Problem

The auth verifier currently accumulates signer weight once per
`DecoratedSignature` entry instead of once per unique signer.

That bug appears in three places:

- `verify_weight_key_claim_internal(...)` accepts repeated signatures for the
  same `public_key_id` inside one weighted auth-claim bucket and adds that
  key's weight multiple times.
- `validate_nonce(...)` iterates all attached signatures and adds source-address
  signer weight once per signature entry.
- `validate_gas_permission(...)` iterates all attached signatures and adds
  sponsor-address signer weight once per signature entry for each matching gas
  permission config.

As a result, one valid signer can be replayed until a weighted threshold is
met.

## Goals

- Reject malformed weighted auth-claim buckets that repeat or reorder signer
  key IDs.
- Preserve current aggregate nonce and gas semantics across multiple auth
  claims while preventing a signer from contributing weight more than once to a
  single aggregate check.
- Keep the fix scoped to verifier behavior. Do not introduce a new
  transaction-wide signature canonicalization rule.

## Non-Goals

- Do not require the full `SignedTransaction::signatures` vector to be sorted.
- Do not prohibit the same key from appearing in multiple distinct auth claims.
  That pattern remains valid; it just must not inflate aggregate weight.
- Do not change cryptographic signature verification or the mapping from
  `auth_claim_idx` to auth claims.

## Design

### 1. Weighted auth claims require strictly increasing `public_key_id`

Within each weighted auth-claim bucket, signatures must appear in strictly
increasing `public_key_id` order.

This check happens in `verify_weight_key_claim_internal(...)`, after validating
that each signer is accepted by the claim bitmap and before any weight is
accumulated.

Behavior:

- If a signer's `public_key_id` is not accepted by the claim bitmap, return
  `kAuthMalformed` as today.
- If a signer's `public_key_id` is less than or equal to the previous signer's
  `public_key_id`, return `kAuthMalformed`.
- Otherwise, proceed normally.

This makes duplicate or out-of-order signers inside one weighted claim
unambiguously malformed. Once that invariant holds, the weighted-claim path can
continue summing once per bucket entry because each entry is now known to refer
to a distinct signer.

### 2. Nonce weight is deduped per source-address signer

`validate_nonce(...)` remains an aggregate check over all signatures attached
to invoked-address auth claims in the transaction.

The function will maintain a `BitMap seen_keys` for source-address signer
contributions:

- If a signature belongs to an invoked-address claim and its key is not allowed
  by `nonce_config.accepted_keys`, return `kAuthDataMismatch` as today.
- If the key has not yet contributed to nonce weight, mark it in `seen_keys`
  and add its configured `max_weight`.
- If the key has already contributed, ignore the duplicate for nonce-weight
  purposes.

This keeps nonce authorization as an address-level property over the attached
claims, while ensuring a key contributes at most once to the nonce threshold.

### 3. Gas permission weight is deduped per config and signer

`validate_gas_permission(...)` also remains an aggregate check over all
signatures whose auth claims belong to the gas sponsor currently being checked.

For each gas-permission config, the function will track a separate `BitMap` of
signers already counted for that config.

Behavior for a signature whose claim address matches the sponsored contract:

- For each gas-permission config that accepts the signer's key:
  - If the key has not yet contributed to that config, mark it seen for that
    config and add its configured `max_weight`.
  - If the key was already seen for that config, ignore it for that config.

This preserves the existing model where one signer may satisfy multiple gas
permission configs if each config accepts that signer, while preventing the
same signer from inflating one config by repetition.

### 4. Error semantics

The fix intentionally uses different semantics for claim-local and
aggregate-address checks:

- Weighted auth-claim buckets:
  - Duplicate or out-of-order `public_key_id` values are `kAuthMalformed`.
- Nonce and gas aggregate checks:
  - Duplicate signer sightings are ignored for weight accumulation.
  - Existing mismatch behavior for unaccepted keys remains unchanged.

This avoids upgrading the bugfix into a broader protocol rule that forbids a
signer from appearing in multiple claims.

## Testing

Add regression tests in `hogd/auth_claim/auth_claim_tests.cc` covering:

1. A weighted claim with repeated `public_key_id` entries in the same bucket
   now fails with `kAuthMalformed`.
2. A weighted claim with out-of-order `public_key_id` entries in the same
   bucket now fails with `kAuthMalformed`.
3. Nonce validation counts the same invoked-address signer only once even if
   that signer appears in multiple source-address claims.
4. Gas-permission validation counts the same sponsor-address signer only once
   per gas-permission config even if that signer appears in multiple
   sponsor-address claims.
5. A positive case showing that the same key may still appear in multiple
   distinct claims without the transaction becoming malformed, provided
   weighted-claim buckets themselves remain strictly increasing.

## Implementation Notes

- The weighted-claim ordering check should stay close to the existing accepted
  key validation in `verify_weight_key_claim_internal(...)`, since both are
  bucket-local structural checks.
- The nonce path needs only one `seen_keys` bitmap because nonce authorization
  is evaluated against one source-address config.
- The gas path needs one `seen_keys` bitmap per gas-permission config because a
  signer can legitimately contribute once to multiple configs.
- No change is needed to how signatures are grouped by `auth_claim_idx`; the
  ordering rule is enforced after bucket formation.
