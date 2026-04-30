# Fix: Empty RCQ Entries Reach Rust SipHash With Null Zero-Length Slice

**Date**: 2026-04-29
**Issue**: [#1612](https://github.com/groundhogchain/hogd/issues/1612)
**Severity**: High
**Approach**: Rust-side fix (Approach B)

## Problem

A contract can insert a zero-length retrocausal queue entry via `RetrocausalQueueInsert`. The syscall only rejects entries exceeding `kQueueEntryMaxLen` (256 bytes), so zero-length input is accepted. `queue_entry_from_span` calls `resize_uninitialized(0)` on a `QueueEntry` (`x23::Vec`), which leaves `data() == nullptr`.

During `RetrocausalQueueLock::commit_change`, `QueueEntryHasher` hashes every inserted entry through `SipHash24` -> `span_to_message_span` -> Rust `siphash24_hash`. The Rust function receives `MessageSpan{nullptr, 0}` and calls `core::slice::from_raw_parts(nullptr, 0)`, which is undefined behavior (Rust requires non-null pointers even for zero-length slices). Debug builds trip the existing `debug_assert!(!data.message_ptr.is_null())`.

Zero-length entries are intentionally allowed as valid signal/ping messages.

## Design

### 1. `MessageSpan::to_slice()` early return for zero-length

**File**: `hogd/rust/crypto/src/lib.rs`

Return `&[]` when `message_len == 0`, bypassing `from_raw_parts`. This makes `MessageSpan` safe for any zero-length input regardless of pointer value.

```rust
pub unsafe fn to_slice(&self) -> &[u8] {
    if self.message_len == 0 {
        return &[];
    }
    core::slice::from_raw_parts(self.message_ptr, self.message_len)
}
```

### 2. Relax debug assertions in Rust FFI entry points

**File**: `hogd/rust/crypto/src/siphash24.rs` (line 26)

Change from:
```rust
debug_assert!(!data.message_ptr.is_null());
```
To:
```rust
debug_assert!(data.message_len == 0 || !data.message_ptr.is_null());
```

**File**: `hogd/rust/crypto/src/ed25519.rs`

Same change for all three FFI entry points that assert `!message.message_ptr.is_null()`:
- `ed25519_dalek_verify` (line 41)
- `ed25519_dalek_sign` (line 135)

(`ed25519_dalek_verify_batch` asserts on the array pointer, not individual message pointers — no change needed there.)

### 3. Rust unit tests

**File**: `hogd/rust/crypto/src/siphash24.rs`

- `test_siphash24_null_zero_length_span`: Hash a `MessageSpan{nullptr, 0}`. Verify no panic and deterministic output (same hash for same key + empty input on repeated calls).

**File**: `hogd/rust/crypto/src/ed25519.rs`

- `test_ed25519_verify_empty_message`: Verify with `MessageSpan{std::ptr::null(), 0}`. Should return an error (bad signature), not panic.

### 4. C++ integration regression test

**File**: `hogd/hogdb/item_lock_test.cc`

Insert an empty `QueueEntry` through `ItemLock::try_atomic_change_lock` and call `commit_change`. After the fix, this should complete without crashing. Verifies the full C++ -> Rust call chain is safe for zero-length entries.

## Files changed

| File | Change |
|------|--------|
| `hogd/rust/crypto/src/lib.rs` | `to_slice()` early return for zero-length |
| `hogd/rust/crypto/src/siphash24.rs` | Relaxed assert + regression test |
| `hogd/rust/crypto/src/ed25519.rs` | Relaxed assert + regression test |
| `hogd/hogdb/item_lock_test.cc` | C++ integration regression test |

## Out of scope

- Rejecting zero-length entries at the syscall boundary (intentionally allowed)
- C++-side `span_to_message_span` sentinel (not needed with Rust-side fix)
- Other `x23::Vec` null-backing-pointer issues unrelated to FFI
