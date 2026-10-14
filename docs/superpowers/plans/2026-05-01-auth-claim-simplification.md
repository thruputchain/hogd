# Auth Claim Simplification Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the 5-variant auth claim system with a single weighted-bitmap claim, remove gas sponsorship and cross-contract auth, simplify the verification pipeline.

**Architecture:** XDR protocol changes cascade through the C++ verification pipeline, auth server, DB layer, syscall layer, genesis, tests, Rust integration contracts, and Go protocol mirror. The `AddressAuthData` struct drops `nonce_config` and `gas_permissions`, gaining a single `required_weight`. The `AuthClaim` union shrinks to `kSourceAddressSignedWeight` (bitmap only) and `kPubkeySignature` (unimplemented placeholder). The syscalls `WriteAddressNonceConfig` and `WriteAddressGasPermissionConfigs` are replaced by `WriteAddressRequiredWeight`.

**Tech Stack:** C++ (x23 XDR), Rust (integration test contracts), Go (protocol mirror)

**Spec:** `docs/superpowers/specs/2026-05-01-auth-claim-simplification-design.md`

---

### Task 1: XDR Protocol Changes

All `.x` file changes. This is the foundation — nothing else compiles until this is done.

**Files:**
- Modify: `protocol/auth_claim.x`
- Modify: `protocol/address_auth.x`
- Modify: `protocol/transaction.x:74-80,200`
- Modify: `protocol/storage_item.x:107-120`
- Modify: `protocol/transaction_trace.x:425-576`
- Modify: `protocol/exec_result.x:19-21`

- [ ] **Step 1: Rewrite `protocol/auth_claim.x`**

Replace the `AuthClaimType` enum and `AuthClaim` union:

```c
enum AuthClaimType {
  kSourceAddressSignedWeight = 0,
  kPubkeySignature = 1
};

union AuthClaim switch (AuthClaimType claim_type) {
case kSourceAddressSignedWeight:
  BitMap accepted_keys;
case kPubkeySignature:
  PubkeyAccountPreimage pubkey_preimage;
};
```

Remove `kMaxGasConfigs` constant. Keep all other types (`Weight`, `BitMap`, `KeyID`, `kMaxSigners`, `kMaxAuthClaims`, `PubkeyAccountPreimage`, `AuthVerifyErrorCode`). Update the module-level comment to reflect the simplified model.

- [ ] **Step 2: Rewrite `protocol/address_auth.x`**

Remove `GasPermissionConfig` struct and `NonceConfig` struct. Replace `AddressAuthData`:

```c
struct AddressAuthData {
  PublicKeyConfigOpt public_keys[kMaxSigners];
  Weight required_weight;
  BlockHeight last_written_height;
};
```

Keep `PublicKeyConfig`, `PublicKeyConfigOpt`, and all default initialization constants. Update the module-level comment.

- [ ] **Step 3: Modify `protocol/transaction.x`**

Remove `GasClaim` struct (lines 74-80) and remove `gas_claims` field from the `Transaction` struct (line 200). Update the `Transaction` doc comment to remove gas claim references.

- [ ] **Step 4: Modify `protocol/storage_item.x`**

Remove `GasConfigChange` struct. Remove `new_nonce_config` and `new_gas_config_change` from `AuthDataChangeArgs`. Add a `Weight* new_required_weight;` field instead:

```c
struct AuthDataChangeArgs {
  AuthDataChangeSigner auth_data_change_signers<>;
  Weight* new_required_weight;
};
```

- [ ] **Step 5: Modify `protocol/transaction_trace.x`**

Remove `SyscallWriteAddressNonceConfigInputs`, `SyscallWriteAddressNonceConfigOutputs`, `SyscallWriteAddressNonceConfigTrace`, `SyscallWriteAddressGasPermissionConfigsInputs`, `SyscallWriteAddressGasPermissionConfigsOutputs`, `SyscallWriteAddressGasPermissionConfigsTrace`.

Remove the `kWriteAddressNonceConfig` and `kWriteAddressGasPermissionConfigs` cases from the `SyscallTrace` union.

Add new types for `WriteAddressRequiredWeight`:

```c
struct SyscallWriteAddressRequiredWeightInputs {
  Weight required_weight;
};

struct SyscallWriteAddressRequiredWeightOutputs {
  void;
};

struct SyscallWriteAddressRequiredWeightTrace {
  SyscallWriteAddressRequiredWeightInputs inputs;
  SyscallWriteAddressRequiredWeightOutputs *outputs;
};
```

Add `kWriteAddressRequiredWeight` case to the `SyscallTrace` union (reuse syscall number 29).

- [ ] **Step 6: Modify `protocol/exec_result.x`**

Remove `ExecResultFailedGasSponsorship = 100` enum value, or leave it as a reserved placeholder with a comment noting it's no longer used. Update the `GasClaim` reference in the comment on line 19-20.

- [ ] **Step 7: Mirror XDR changes to `groundhog-go/protocol-go/protocol/`**

Apply the same changes to the Go protocol mirror files:
- `groundhog-go/protocol-go/protocol/auth_claim.x`
- `groundhog-go/protocol-go/protocol/address_auth.x`
- `groundhog-go/protocol-go/protocol/transaction.x`
- `groundhog-go/protocol-go/protocol/storage_item.x`
- `groundhog-go/protocol-go/protocol/transaction_trace.x`
- `groundhog-go/protocol-go/protocol/exec_result.x` (if present)

- [ ] **Step 8: Commit**

```bash
git add protocol/ groundhog-go/protocol-go/protocol/
git commit -m "refactor(protocol): simplify auth claims to single weighted bitmap"
```

---

### Task 2: Verification Pipeline Rewrite

Rewrite `verify.h` and `verify.cc` for the new claim types.

**Files:**
- Modify: `hogd/auth_claim/verify.h`
- Modify: `hogd/auth_claim/verify.cc`

- [ ] **Step 1: Rewrite `hogd/auth_claim/verify.h`**

Remove:
- `calculate_source_address_implicit_gas_claim` function
- `get_aux_addresses` function
- `aux_auth_data` parameter from `lightweight_validate_auth_claims` and `verify_auth`

New signatures:

```cpp
AuthVerifyStatus<void> auth_claim_is_well_formed(const SignedTransaction &stx);

AuthVerifyStatus<void> lightweight_validate_auth_claims(
    const SignedTransaction &stx, const AddressAuthData &source_auth_data);

AuthVerifyStatus<void>
verify_auth(const SignedTransaction &stx,
            const AddressAuthData &source_auth_data,
            Ed25519SignatureVerifier *signature_verifier = nullptr);
```

Keep `Ed25519SignatureVerifier` unchanged.

- [ ] **Step 2: Rewrite `hogd/auth_claim/verify.cc`**

Remove:
- `verify_individual_key_claim_internal` template
- `validate_nonce` template
- `validate_gas_permission` template
- All `kOtherAddress*` and `kInvokedAddress*` branches

Keep internal helpers: `valid_pk_claim`, `valid_pk_bitmap`, `bitmap_accepts_key`, `has_public_key`, `verify_decorated_signature`, `VerificationType` enum.

Rewrite `verify_weight_key_claim_internal` — it now takes only `BitMap accepted_keys` (from the claim), no `required_weight` param. The weight check happens at the end of `verify_auth_claims_internal` against `source_auth_data.required_weight` instead of per-claim.

Rewrite `verify_auth_claim_internal` to handle only two cases:
- `kSourceAddressSignedWeight`: call `verify_weight_key_claim_internal` with `claim.accepted_keys()`.
- `kPubkeySignature`: return `kAuthMalformed` (unimplemented).

Rewrite `verify_auth_claims_internal`:
- Remove `aux_auth_data` parameter.
- For `kSourceAddressSignedWeight` claims, always use `source_auth_data`.
- After verifying all claims, accumulate total weight from all signatures across all claims (using `source_auth_data.public_keys[sig.public_key_id]->max_weight`), check `>= source_auth_data.required_weight`.
- Remove gas claim validation block entirely.
- Remove nonce validation call.

Update the three public functions (`auth_claim_is_well_formed`, `lightweight_validate_auth_claims`, `verify_auth`) to match new signatures.

- [ ] **Step 3: Commit**

```bash
git add hogd/auth_claim/verify.h hogd/auth_claim/verify.cc
git commit -m "refactor(auth): simplify verification pipeline for single claim type"
```

---

### Task 3: Auth Server Simplification

Remove auxiliary address lookups from the auth server.

**Files:**
- Modify: `hogd/auth/auth_server.cc`

- [ ] **Step 1: Simplify `AuthServer::verify_transactions`**

In the auth data pre-fetching loop (around line 220): remove the `get_aux_addresses` call and the loop that fetches auxiliary auth data.

In the verification loop (around line 260): remove `get_aux_addresses`, `aux_auth_data` map construction, and pass only `source_auth_data` to `verify_auth`.

- [ ] **Step 2: Simplify `AuthServer::verify_batch`**

Around line 305: remove `get_aux_addresses` call, `aux_owners` vector, `aux_auth_data` map construction, and the loop that populates them. Pass only `*source_auth_data` to `verify_auth`.

- [ ] **Step 3: Commit**

```bash
git add hogd/auth/auth_server.cc
git commit -m "refactor(auth): remove auxiliary address lookups from auth server"
```

---

### Task 4: DB Layer Cleanup

Remove nonce/gas config from the DB change pipeline.

**Files:**
- Modify: `hogd/hogdb/item_change.h:376-407`
- Modify: `hogd/hogdb/item_lock/auth_lock.h`
- Modify: `hogd/hogdb/item_lock/auth_lock.cc`
- Modify: `hogd/hogdb/invoke_tx.cc:450-462`
- Modify: `hogd/hogdb/rpc_from_sched.cc:380-402`

- [ ] **Step 1: Modify `hogd/hogdb/item_change.h`**

In `AuthDataItem`:
- Remove `set_new_nonce` method
- Remove `set_new_gas_configs` method
- Remove `nonce_change_` member
- Remove `gas_config_change_` member
- Add `set_new_required_weight(Weight w)` method and `std::optional<Weight> required_weight_change_` member

- [ ] **Step 2: Modify `hogd/hogdb/item_lock/auth_lock.h`**

Remove `nonce_config_` and `gas_permission_configs_` members. Add `std::optional<Weight> required_weight_change_`.

- [ ] **Step 3: Modify `hogd/hogdb/item_lock/auth_lock.cc`**

In `build_result()`: replace `nonce_config_` and `gas_permission_configs_` assignment with `required_weight_change_` assignment to `result.required_weight`.

In `commit_change()`: replace `nonce_change_` and `gas_config_change_` handling with `required_weight_change_`.

- [ ] **Step 4: Modify `hogd/hogdb/invoke_tx.cc`**

In `request_auth_data` (around line 457-462): replace `new_nonce_config` and `new_gas_config_change` handling with `new_required_weight`:

```cpp
if (auth_data_change.new_required_weight) {
    auth_data_item.set_new_required_weight(*auth_data_change.new_required_weight);
}
```

- [ ] **Step 5: Modify `hogd/hogdb/rpc_from_sched.cc`**

In the genesis auth data setup (around line 391-401): remove gas config change construction and `new_nonce_config` setting. Replace with:

```cpp
AuthDataChangeArgs auth_data_change = {
    .auth_data_change_signers = auth_data_change_signers,
    .new_required_weight = auth_data.required_weight,
};
```

- [ ] **Step 6: Commit**

```bash
git add hogd/hogdb/item_change.h hogd/hogdb/item_lock/ hogd/hogdb/invoke_tx.cc hogd/hogdb/rpc_from_sched.cc
git commit -m "refactor(hogdb): replace nonce/gas config with required_weight in auth data pipeline"
```

---

### Task 5: Syscall Layer Cleanup

Remove nonce/gas config syscalls, add required_weight syscall. Update FFI helpers.

**Files:**
- Modify: `hogd/hogexec/runtime/syscall_auth.cc`
- Modify: `hogd/hogexec/runtime/syscall_handler.cc:1413-1460`
- Modify: `hogd/hogexec/runtime/syscall_handler.h:326-329`
- Modify: `hogd/hogexec/runtime/ffi_helpers.h`
- Modify: `hogd/hogexec/runtime/item/item_view.h:183-204`
- Modify: `hogd/hogexec/runtime/item/item_view.cc:444-469`

- [ ] **Step 1: Modify `hogd/hogexec/runtime/ffi_helpers.h`**

Update `FFIHelperType<generated::AuthClaim>`:
- `to_cpp`: handle `kSourceAddressSignedWeight` (set `claim.accepted_keys()` from the ffi bitmap) and `kPubkeySignature`. Remove all old claim type cases.
- `to_ffi`: handle `kSourceAddressSignedWeight` and `kPubkeySignature`. Remove all old claim type cases.

Remove `FFIHelperType<generated::NonceConfig>` specialization entirely.
Remove `FFIHelperType<generated::GasPermissionConfig>` specialization entirely.
Remove corresponding `static_assert` lines.

- [ ] **Step 2: Modify `hogd/hogexec/runtime/item/item_view.h`**

In `AddressAuthDataItem`:
- Remove `set_nonce_config` method
- Remove `set_gas_permission_configs` method
- Add `set_required_weight(Weight w)` method

In `InternalAuthData`:
- Remove `gas_permission_configs_` member
- Remove `nonce_config_` member
- Add `std::optional<Weight> required_weight_` member

- [ ] **Step 3: Modify `hogd/hogexec/runtime/item/item_view.cc`**

In `AddressAuthDataItem::get_delta()`:
- Remove nonce config delta code (lines 454-457)
- Remove gas permission config delta code (lines 458-462)
- Add required_weight delta: if `internal_auth_data_->required_weight_.has_value()`, set `delta.auth_data_change().new_required_weight = internal_auth_data_->required_weight_.value()`.

- [ ] **Step 4: Modify `hogd/hogexec/runtime/syscall_auth.cc`**

Remove `syscall_WriteAddressNonceConfig_inner` function entirely (lines 119-140).
Remove `syscall_WriteAddressGasPermissionConfigs_inner` function entirely (lines 142-203).

Add new `syscall_WriteAddressRequiredWeight_inner`:

```cpp
std::pair<HostFnStatus<SyscallErrorCode>, WrappedGasAmount>
syscall_WriteAddressRequiredWeight_inner(
    SystemCallHandler &syscall_handler,
    Weight input_required_weight) noexcept
{
  auto status = syscall_handler.set_address_required_weight(input_required_weight);
  if (!status) {
    return {status.error().to_host_fn_error(), WrappedGasAmount{0}};
  }
  if (!status.value()) {
    return {status.value().error().code(), WrappedGasAmount{0}};
  }
  return {SyscallErrorCode::kSyscallErrorCodeNone, WrappedGasAmount{0}};
}
```

- [ ] **Step 5: Modify `hogd/hogexec/runtime/syscall_handler.h`**

Remove `set_address_nonce_config` and `set_address_gas_permission_configs` declarations.
Add:

```cpp
SystemStatus<SyscallStatus<void>> set_address_required_weight(Weight required_weight);
```

- [ ] **Step 6: Modify `hogd/hogexec/runtime/syscall_handler.cc`**

Remove `set_address_nonce_config` and `set_address_gas_permission_configs` implementations.
Add `set_address_required_weight` following the same pattern:

```cpp
SystemStatus<SyscallStatus<void>>
SystemCallHandler::set_address_required_weight(Weight required_weight)
{
  auto addr = get_current_address();
  auto addrkey = auth_data_addrkey(addr);
  auto *item_view = invocation_frame_stack_.get_item_view(addrkey);
  assert(item_view);

  if (auto *address_auth_data_item = item_view->get<AddressAuthDataItem>()) {
    address_auth_data_item->set_required_weight(required_weight);
    return {};
  }

  if (item_view->get<DeletedItem>()) [[unlikely]] {
    error("Unrecoverable: unexpected manual delete of authentication data");
    return SystemError{SystemErrorCode::Unrecoverable};
  }
  assert(item_view->get<UnmodifiedItem>());
  AddressAuthDataItem new_item;
  new_item.set_required_weight(required_weight);
  return item_view->set(new_item);
}
```

- [ ] **Step 7: Update syscall registration**

The generated `syscall.x` is auto-generated. Find where syscall numbers/names are defined and replace `kWriteAddressNonceConfig = 29` with `kWriteAddressRequiredWeight = 29`. Remove `kWriteAddressGasPermissionConfigs = 30` (or mark as reserved). Update `kSyscallCount` accordingly. Also update the `SyscallMeta` specialization if one exists for these syscalls.

- [ ] **Step 8: Update exec_result references**

In `hogd/hogexec/interface/executor_context.cc`, the `ExecResultFailedGasSponsorship` case just falls through to "nothing to do". If the enum value was removed, remove this case. If kept as reserved, leave it.

- [ ] **Step 9: Commit**

```bash
git add hogd/hogexec/runtime/ hogd/hogexec/interface/
git commit -m "refactor(syscall): replace nonce/gas config syscalls with WriteAddressRequiredWeight"
```

---

### Task 6: Genesis Configuration Update

Update genesis to use simplified auth data.

**Files:**
- Modify: `hogd/genesis/genesis_configuration.cc:99-102`

- [ ] **Step 1: Modify genesis auth data initialization**

Replace:
```cpp
deployer_auth_data.nonce_config.accepted_keys = 0;
deployer_auth_data.nonce_config.required_weight = 0;
deployer_auth_data.gas_permissions.push_back(
    GasPermissionConfig{.limit = std::numeric_limits<GasAmount>::max(),
                        ...});
```

With:
```cpp
deployer_auth_data.required_weight = 0;
```

- [ ] **Step 2: Commit**

```bash
git add hogd/genesis/genesis_configuration.cc
git commit -m "refactor(genesis): use simplified auth data initialization"
```

---

### Task 7: Test FFI Bindings Update

Update the test bindings that bridge C++ auth verification for testing.

**Files:**
- Modify: `hogd/test_ffi/test_bindings.h`
- Modify: `hogd/test_ffi/test_bindings.cc`
- Modify: `hogd/test_ffi/test_bindings_test.cc`

- [ ] **Step 1: Modify `hogd/test_ffi/test_bindings.h`**

Remove `FFIAuxiliaryAuthData` parameter from the auth verification function signature. The function should only take the signed transaction and source auth data.

- [ ] **Step 2: Modify `hogd/test_ffi/test_bindings.cc`**

Remove `aux_auth_data` handling — the loop that deserializes auxiliary auth data and builds the `aux_auth_data` map (around lines 756-769). Update the call to `verify_auth` to pass only source auth data.

- [ ] **Step 3: Rewrite `hogd/test_ffi/test_bindings_test.cc`**

Update all tests to:
- Use `kSourceAddressSignedWeight` instead of `kInvokedAddressSignature`/etc.
- Remove `nonce_config` and `gas_permissions` from `AddressAuthData` construction
- Set `required_weight` instead
- Remove `aux_auth_data` from verification calls
- Remove `make_aux_auth_data` helper

- [ ] **Step 4: Commit**

```bash
git add hogd/test_ffi/
git commit -m "refactor(test_ffi): update bindings for simplified auth claims"
```

---

### Task 8: Auth Claim Verification Tests Rewrite

Rewrite the auth claim unit tests.

**Files:**
- Modify: `hogd/auth_claim/auth_claim_tests.cc`

- [ ] **Step 1: Rewrite test helpers**

Remove `kAnyKeyNonceConfig`. All `AddressAuthData` construction should use the new struct (no `nonce_config`, no `gas_permissions`, just `public_keys` + `required_weight`).

- [ ] **Step 2: Rewrite well-formedness tests**

Update all tests to use `kSourceAddressSignedWeight` claims. Test:
- Valid bitmap + valid signatures → pass
- Invalid bitmap (bits beyond kMaxSigners) → `kAuthMalformed`
- Signature key_id not in bitmap → `kAuthMalformed`
- Non-strictly-increasing key IDs → `kAuthMalformed`
- `kPubkeySignature` → `kAuthMalformed`

- [ ] **Step 3: Rewrite lightweight validation tests**

Test weight accumulation:
- Signatures with enough weight → pass
- Signatures with insufficient weight → `kAuthDataMismatch`
- Signature references non-existent key → `kAuthDataMismatch`

- [ ] **Step 4: Remove gas/nonce/cross-contract tests**

Delete all tests for:
- `kOtherAddressSignature` / `kOtherAddressSignedWeight`
- `kInvokedAddressSignature` (single-key variant)
- `validate_nonce` behavior
- `validate_gas_permission` behavior
- Gas claim validation

- [ ] **Step 5: Commit**

```bash
git add hogd/auth_claim/auth_claim_tests.cc
git commit -m "test(auth): rewrite auth claim tests for simplified claim types"
```

---

### Task 9: Auth Server Tests Rewrite

Rewrite the auth server verification tests.

**Files:**
- Modify: `hogd/auth/auth_verify_test.cc`

- [ ] **Step 1: Rewrite auth data setup**

Replace all `AddressAuthData` construction:
- Remove `nonce_config` and `gas_permissions` fields
- Set `required_weight` instead

- [ ] **Step 2: Rewrite claim construction**

Replace `kInvokedAddressSignature` claims with `kSourceAddressSignedWeight` claims. Remove any auxiliary auth data setup and `aux_auth_data` map construction.

- [ ] **Step 3: Update verification calls**

Remove `aux_auth_data` parameter from all `verify_auth` / `lightweight_validate_auth_claims` calls.

- [ ] **Step 4: Commit**

```bash
git add hogd/auth/auth_verify_test.cc
git commit -m "test(auth): rewrite auth server tests for simplified claims"
```

---

### Task 10: Syscall Auth Config Tests Rewrite

Rewrite or remove syscall auth config tests.

**Files:**
- Modify: `hogd/hogexec/runtime/syscall_auth_config_test.cc`

- [ ] **Step 1: Remove gas permission config tests**

Delete all `WriteAddressGasPermissionConfigs*` tests:
- `WriteAddressGasPermissionConfigsSuccess`
- `WriteAddressGasPermissionConfigsEmpty`
- `WriteAddressGasPermissionConfigsGapError`
- `WriteAddressGasPermissionConfigsGapAtEnd`
- `WriteAddressGasPermissionConfigsAllFive`
- `WriteAddressGasPermissionConfigsInvalidBitmap`

- [ ] **Step 2: Replace nonce config tests with required_weight tests**

Replace `WriteAddressNonceConfigSuccess` and `WriteAddressNonceConfigInvalidBitmap` with tests for `WriteAddressRequiredWeight`:
- Valid weight → success
- Verify the weight is stored in auth data

- [ ] **Step 3: Rewrite combined test**

The test at line 429 that exercises `ChangePublicKeyRegistration`, `WriteAddressNonceConfig`, and `WriteAddressGasPermissionConfigs` together should be rewritten to exercise `ChangePublicKeyRegistration` and `WriteAddressRequiredWeight` together.

- [ ] **Step 4: Commit**

```bash
git add hogd/hogexec/runtime/syscall_auth_config_test.cc
git commit -m "test(syscall): rewrite auth config tests for WriteAddressRequiredWeight"
```

---

### Task 11: DB Layer Tests Update

Update hogdb and auth lock tests.

**Files:**
- Modify: `hogd/hogdb/hogdb_test.cc`
- Modify: `hogd/hogdb/item_lock/auth_lock_test.cc`
- Modify: `hogd/hogexec/runtime/authclaim_test.cc`
- Modify: `hogd/hogexec/runtime/ffi_helpers_tests.cc`
- Modify: `hogd/hogexec/runtime/syscall_misc_tests.cc`

- [ ] **Step 1: Update `hogd/hogdb/hogdb_test.cc`**

Replace all `new_nonce_config` usage with `new_required_weight`. Remove gas config references.

- [ ] **Step 2: Rewrite `hogd/hogdb/item_lock/auth_lock_test.cc`**

- `SetNonceConfig` test → `SetRequiredWeight` test
- `SetGasPermissionConfigs` test → delete
- `NonceConfigRollback` test → `RequiredWeightRollback` test (or simplify)

- [ ] **Step 3: Update `hogd/hogexec/runtime/authclaim_test.cc`**

Rewrite to use `kSourceAddressSignedWeight` claims. Remove `kOtherAddressSignature` and `kOtherAddressSignedWeight` claim construction. The test constructs claims and checks their FFI representation.

- [ ] **Step 4: Update `hogd/hogexec/runtime/ffi_helpers_tests.cc`**

Rewrite FFI roundtrip tests for `kSourceAddressSignedWeight` only. Remove tests for old claim types.

- [ ] **Step 5: Update `hogd/hogexec/runtime/syscall_misc_tests.cc`**

Around line 521-530: the `WriteAddressGasPermissionConfigs` error test. Remove or replace with a test for the new syscall.

- [ ] **Step 6: Commit**

```bash
git add hogd/hogdb/hogdb_test.cc hogd/hogdb/item_lock/auth_lock_test.cc
git add hogd/hogexec/runtime/authclaim_test.cc hogd/hogexec/runtime/ffi_helpers_tests.cc
git add hogd/hogexec/runtime/syscall_misc_tests.cc
git commit -m "test(hogdb): update DB and runtime tests for auth simplification"
```

---

### Task 12: Genesis and Loadgen Tests

Update genesis tests, loadgen, and mempool test helpers.

**Files:**
- Modify: `hogd/genesis/genesis_configuration_test.cc`
- Modify: `hogd/genesis/genesis_configuration_unfurl_tests.cc`
- Modify: `hogd/loadgen/erc20_basic_auth_experiment.cc`
- Modify: `hogd/loadgen/loadgen_tests.cc`
- Modify: `hogd/mempool/test/txs.h`
- Modify: `hogd/mempool/test/mempool_test.cc`

- [ ] **Step 1: Update `hogd/genesis/genesis_configuration_test.cc`**

Replace `nonce_config.required_weight` and `gas_permissions` assertions with `required_weight` assertion.

- [ ] **Step 2: Update `hogd/genesis/genesis_configuration_unfurl_tests.cc`**

Replace `deployer_auth_data.nonce_config.*` and `gas_permissions` setup with `deployer_auth_data.required_weight = 0`.

- [ ] **Step 3: Update `hogd/loadgen/erc20_basic_auth_experiment.cc`**

Replace `kInvokedAddressSignature` claim construction with `kSourceAddressSignedWeight` claims. The bitmap should have the bit set for the signing key.

- [ ] **Step 4: Update `hogd/loadgen/loadgen_tests.cc`**

Replace auth data setup (remove `nonce_config`, `gas_permissions`, set `required_weight`).

- [ ] **Step 5: Update `hogd/mempool/test/txs.h`**

- Remove `gas_claims` from transaction construction (lines 43, 62)
- Replace `kNonceConfig` with setting `required_weight` on auth data
- Replace `kInvokedAddressSignature` claim with `kSourceAddressSignedWeight`
- Remove `add_nonce_key` helper that modifies `nonce_config.accepted_keys`
- Remove `add_gas_permission` helper that modifies `gas_permissions`
- Remove `generate_change_nonce_config` helper

- [ ] **Step 6: Update `hogd/mempool/test/mempool_test.cc`**

Replace `auth_data.nonce_config.required_weight` assignments and `generate_change_nonce_config` calls with the new APIs.

- [ ] **Step 7: Commit**

```bash
git add hogd/genesis/ hogd/loadgen/ hogd/mempool/test/
git commit -m "test: update genesis, loadgen, and mempool tests for auth simplification"
```

---

### Task 13: Integration Test Contracts (Rust)

Update the Rust integration test contracts.

**Files:**
- Modify: `built_contracts/integration_test_contracts/authclaim_test/src/implementation.rs`
- Modify: `built_contracts/integration_test_contracts/authclaim_test/src/interface.rs`
- Modify: `built_contracts/integration_test_contracts/fibonacci/src/implementation.rs`
- Modify: `built_contracts/integration_test_contracts/fibonacci/src/interface.rs`
- Modify: `built_contracts/integration_test_contracts/simple_wallet_authclaim/src/implementation.rs`
- Modify: `built_contracts/integration_test_contracts/return_fault_test/src/interface.rs`
- Modify: `built_contracts/integration_test_contracts/syscall_misc_tests/src/implementation.rs`

- [ ] **Step 1: Rewrite `authclaim_test` contract**

Update `implementation.rs` to handle only `AuthClaim::kSourceAddressSignedWeight` (reading `accepted_keys` bitmap). Remove handlers for old claim types. Update `interface.rs` doc comments.

- [ ] **Step 2: Update `fibonacci` contract**

Remove `change_nonce_config` function and its `use sdk::NonceConfig` import. Replace with a `change_required_weight(required_weight: u32)` function that calls the new SDK API (this depends on the Rust SDK being updated — if it's external, add a TODO).

- [ ] **Step 3: Update `simple_wallet_authclaim` contract**

Remove `sdk::set_nonce_config` and `sdk::set_gas_permission_config` calls. Remove `use sdk::NonceConfig` and `use sdk::GasPermissionConfig`. Replace auth claim matching with `kSourceAddressSignedWeight`.

- [ ] **Step 4: Update `return_fault_test` contract**

Update interface doc comment from `kInvokedAddressSignature` to `kSourceAddressSignedWeight`.

- [ ] **Step 5: Update `syscall_misc_tests` contract**

Remove or update the `WriteAddressGasPermissionConfigs` error test (around line 521-530).

- [ ] **Step 6: Commit**

```bash
git add built_contracts/integration_test_contracts/
git commit -m "refactor(contracts): update integration test contracts for simplified auth"
```

---

### Task 14: Build and Verify

Build the project and fix any remaining compilation errors.

- [ ] **Step 1: Build**

Run the build using the project's build skill. Fix any remaining compilation errors that were missed.

- [ ] **Step 2: Run tests**

Run the auth-related test suites:
- `auth_claim_tests`
- `auth_verify_test`
- `syscall_auth_config_test`
- `auth_lock_test`
- `hogdb_test`
- `test_bindings_test`
- `ffi_helpers_tests`
- `authclaim_test`
- `genesis_configuration_test`
- `mempool_test`

- [ ] **Step 3: Fix any test failures**

- [ ] **Step 4: Final commit**

```bash
git commit -m "fix: resolve remaining compilation and test issues"
```
