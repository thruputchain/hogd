# Regression Tests: Read-Only Invoke Drops Unreturned Forwarded Auth Tokens

**Date**: 2026-05-25
**Issue**: #1765
**Disposition**: WAI (Working As Intended)

## Context

When a contract forwards auth tokens into a read-only invoke, unreturned tokens are lost after the child frame commits successfully. Issue #1765 flagged this as a security concern, but the behavior is WAI:

- There is no normal case where a contract forwards tokens to a read-only call expecting them back.
- Contracts are always allowed to drop auth tokens (by not returning them from any invoke).
- This is equivalent to the caller voluntarily discarding its own tokens.

These regression tests codify the WAI behavior so future changes don't accidentally alter it.

## Test Plan

### Test 1: Proposal mode — `ReadOnlyInvokeDropsUnreturnedForwardedAuthTokens`

**File**: `hogd/hogexec/runtime/syscall_auth_token_test.cc`
**Fixture**: `SyscallAuthTokenTest` (existing, proposal mode + HogDB)

Three native contracts:

- **Caller** (address 0): Mints a token targeting child, forwards it via `kInvoke` with `InvokeFlag::ReadOnly`. After the read-only child returns, attempts to forward the same token to a consumer. Asserts the second forward fails with `kSyscallErrorCodeInvalidSyscallInput`.
- **Read-only child** (address 1): Receives the token but does not consume or return it. Succeeds normally.
- **Consumer** (address 2): Should never be reached; fails the test if invoked.

Assertions:
- Read-only invoke succeeds (`kSyscallErrorCodeNone`).
- Second invoke (forwarding the now-dropped token) returns `kSyscallErrorCodeInvalidSyscallInput`.
- `root_invoke()` succeeds (the invalid-input error is a syscall-level error, not a frame fault).
- `try_commit_transaction()` returns `ExecResultOK`.

### Test 2: Viewing mode — `ViewReadOnlyInvokeDropsUnreturnedForwardedAuthTokens`

**File**: `hogd/hogexec/runtime/view/view_test.cc`
**Fixture**: `ViewTest` (existing)

Two native contracts:

- **Outer** (address 10): Receives a forwarded token from the view query's `forwarded_tokens`. Invokes a read-only child, forwarding the token. After the child returns, attempts to forward the token again to a third contract. The second forward should fail.
- **Read-only child** (address 11): Receives the token, does not consume or return it, succeeds.

The view query supplies a `parent_address` and a single `AuthToken` in `forwarded_tokens`.

Assertions:
- `run_view()` returns `TransactionViewStatus::Ok`.
- Output `status_code` is `InvocationFrameStatusCode::Return`.

## Implementation

Both tests use only existing test infrastructure — `NativeContract` lambdas, `SyscallAuthTokenTest` fixture, `ViewTest::NativeViewSetup`, and existing syscall wrappers. No new helpers or contracts needed.
