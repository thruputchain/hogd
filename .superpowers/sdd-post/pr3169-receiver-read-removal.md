# PR #3169 Post-Review: Receiver-Side Balance Read Removal

**Date:** 2026-08-04
**Branch:** geoff/native-token-strict-erc20-parity

## Summary

All four tasks from the post-review spec were applied, built, and tested successfully.

## Changes Applied

### 1. Remove Receiver-Side Balance Read (both impls)

**File:** `hogd/hogexec/precompiles/precompile_native_token.cc`

In both `transfer_impl` and `transfer_from_impl`, deleted the entire credit-leg
block: the `int128_t receiver_avail` declaration, the `receiver == owner`
post-debit special case, the `get_native_token_balance(receiver)` read with its
unwrap ladder, and the `integer_add_overflow(receiver_avail, amount_signed_int)`
check. Replaced with the supply-bound rationale comment in each site.

### 2. CreditOverflowTransferParity → CreditOverflowBeyondSupplyDiverges

**File:** `hogd/hogexec/runtime/erc20_parity_test.cc`

Renamed the test and replaced the byte-identical assertions with divergence
assertions: native record is 1 byte (non-OK commit), WASM record has ≥3 bytes
with frame Abort at index 2, and the two differ. Comment documents this as a
known supply-cap-unreachable divergence.

### 3. Small Fixes

- `native_token_allowance_test.cc` ~line 627: stale comment "(Balance, unlike
  allowance, stays deferred...)" reworded to "Balance checks are also strict..."
- `precompile_native_token.cc`: all three occurrences of "in-tx increments" /
  "accumulated increments" changed to "net in-tx delta" (allowance `adjust_allowance`
  and both strict-balance-check comments).
- `balance_cost_tracking_test.cc` line 785: "in-tx increments" → "net in-tx delta".
- `native_token_unchecked_transfer`: `CHECK(amount >= 0)` replaced with
  `if (amount < 0) { return InvokeError{InvokeErrorCode::Deterministic}; }`.

### 4. Distinct-Recipient transfer_from Parity Coverage

**File:** `hogd/hogexec/runtime/erc20_parity_test.cc`

Added:
- Two new fixture addresses: `kNativeTfRecipientAddr` (210) and
  `kWasmTfRecipientAddr` (211).
- Helper method `run_transfer_from_parity(owner_balance, allowance,
  transfer_amount)` mirroring `run_transfer_from_self_parity` but sending to a
  distinct recipient.
- `FundedTransferFromToRecipientParity`: balance=1000, allowance=200, amount=100
  → byte-identical, frame Return.
- `OneOverBalanceTransferFromToRecipientParity`: balance=100, allowance=200,
  amount=101 → byte-identical, frame Abort.

## Build & Test

- Build: `cmake --build ./build -j16` succeeded (compile errors: 0).
  CMake test-discovery timed out on `runtime_test` during the full build,
  deleting the binary; rebuilt with `--target runtime_test` which succeeded.
- Targeted filter `Erc20ParityTest.*:NativeTokenAllowance*:BalanceCostTrackingTest.*`:
  **44/44 passed**.
- Full `runtime_test` suite: **710 passed, 36 skipped (pre-existing
  MockInMemory skips), 1 disabled, 0 failed** (746 total).
- `ctest -R GenesisConfigurationUnfurl`: **7/7 passed**.
- clang-format-21 applied; minor whitespace reformatting in `erc20_parity_test.cc`.

## Deviations from Spec

None.
