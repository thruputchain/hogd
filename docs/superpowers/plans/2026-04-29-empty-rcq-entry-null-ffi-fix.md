# Empty RCQ Entry Null FFI Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix contract-triggerable Rust FFI undefined behavior when zero-length RCQ entries pass a null pointer through `MessageSpan` into `core::slice::from_raw_parts`.

**Architecture:** Rust-side fix. `MessageSpan::to_slice()` gets an early return for zero-length spans, avoiding `from_raw_parts(nullptr, 0)`. Debug assertions in all FFI entry points are relaxed to allow null-with-zero-length. Regression tests at both the Rust unit level and C++ integration level.

**Tech Stack:** Rust (crypto_lib crate), C++ (gtest), CMake build via `scripts/build.sh`

---

## File Map

| File | Action | Responsibility |
|------|--------|----------------|
| `hogd/rust/crypto/src/lib.rs` | Modify | `MessageSpan::to_slice()` early return for zero-length |
| `hogd/rust/crypto/src/siphash24.rs` | Modify | Relax assert + add regression test |
| `hogd/rust/crypto/src/ed25519.rs` | Modify | Relax asserts + add regression test |
| `hogd/hogdb/item_lock_test.cc` | Modify | C++ integration regression test |

---

### Task 1: Fix `MessageSpan::to_slice()` and add SipHash regression test

**Files:**
- Modify: `hogd/rust/crypto/src/lib.rs:11-18`
- Modify: `hogd/rust/crypto/src/siphash24.rs:26`
- Test: `hogd/rust/crypto/src/siphash24.rs` (inline test module)

- [ ] **Step 1: Write the failing Rust test for siphash24 with null zero-length span**

Add this test to the `mod tests` block at the bottom of `hogd/rust/crypto/src/siphash24.rs` (after the existing `test_siphash24_same_input_same_output` test, before the closing `}`):

```rust
    #[test]
    fn test_siphash24_null_zero_length_span() {
        let result1 = unsafe {
            siphash24_hash(
                1,
                2,
                MessageSpan {
                    message_ptr: std::ptr::null(),
                    message_len: 0,
                },
            )
        };

        let result2 = unsafe {
            siphash24_hash(
                1,
                2,
                MessageSpan {
                    message_ptr: std::ptr::null(),
                    message_len: 0,
                },
            )
        };

        assert_eq!(result1, result2);
    }
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd hogd/rust && cargo test -p crypto_lib -- test_siphash24_null_zero_length_span`

Expected: FAIL — debug assertion panics on `!data.message_ptr.is_null()`.

- [ ] **Step 3: Fix `MessageSpan::to_slice()` in `hogd/rust/crypto/src/lib.rs`**

Replace the `to_slice` method (lines 15-17) with:

```rust
    pub unsafe fn to_slice(&self) -> &[u8] {
        if self.message_len == 0 {
            return &[];
        }
        core::slice::from_raw_parts(self.message_ptr, self.message_len)
    }
```

- [ ] **Step 4: Relax the debug assertion in `hogd/rust/crypto/src/siphash24.rs`**

Replace line 26:
```rust
    debug_assert!(!data.message_ptr.is_null());
```
With:
```rust
    debug_assert!(data.message_len == 0 || !data.message_ptr.is_null());
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cd hogd/rust && cargo test -p crypto_lib -- test_siphash24_null_zero_length_span`

Expected: PASS

- [ ] **Step 6: Run all existing siphash tests to check for regressions**

Run: `cd hogd/rust && cargo test -p crypto_lib -- siphash24`

Expected: All 4 tests pass (3 existing + 1 new).

- [ ] **Step 7: Commit**

```bash
git add hogd/rust/crypto/src/lib.rs hogd/rust/crypto/src/siphash24.rs
git commit -m "fix: make MessageSpan::to_slice() safe for null zero-length spans (#1612)

Return &[] for zero-length spans, bypassing from_raw_parts(nullptr, 0).
Relax siphash24_hash debug_assert to allow null when len == 0."
```

---

### Task 2: Relax ed25519 assertions and add regression test

**Files:**
- Modify: `hogd/rust/crypto/src/ed25519.rs:41,135`
- Test: `hogd/rust/crypto/src/ed25519.rs` (inline test module)

- [ ] **Step 1: Write the failing Rust test for ed25519 verify with null zero-length message**

Add this test to the `mod tests` block at the bottom of `hogd/rust/crypto/src/ed25519.rs` (after the existing `test_ed25519_sign_and_verify` test, before the closing `}`):

```rust
    #[test]
    fn test_ed25519_verify_empty_message() {
        let mut csprng = OsRng;
        let mut secret_key_bytes = [0u8; 32];
        csprng.fill_bytes(&mut secret_key_bytes);
        let signing_key = SigningKey::from_bytes(&secret_key_bytes);
        let verifying_key: [u8; 32] = signing_key.verification_key().into();

        let fake_signature = [0u8; 64];

        let result = unsafe {
            ed25519_dalek_verify(
                &verifying_key as *const [u8; 32],
                &fake_signature as *const [u8; 64],
                MessageSpan {
                    message_ptr: std::ptr::null(),
                    message_len: 0,
                },
            )
        };

        assert_eq!(result, Ed25519Result::VerificationFailed);
    }
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd hogd/rust && cargo test -p crypto_lib -- test_ed25519_verify_empty_message`

Expected: FAIL — debug assertion panics on `!message.message_ptr.is_null()`.

- [ ] **Step 3: Relax the debug assertions in `hogd/rust/crypto/src/ed25519.rs`**

Replace line 41:
```rust
    debug_assert!(!message.message_ptr.is_null());
```
With:
```rust
    debug_assert!(message.message_len == 0 || !message.message_ptr.is_null());
```

Replace line 135:
```rust
    debug_assert!(!message.message_ptr.is_null());
```
With:
```rust
    debug_assert!(message.message_len == 0 || !message.message_ptr.is_null());
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cd hogd/rust && cargo test -p crypto_lib -- test_ed25519_verify_empty_message`

Expected: PASS — returns `VerificationFailed` (bad signature), no panic.

- [ ] **Step 5: Run all existing ed25519 tests to check for regressions**

Run: `cd hogd/rust && cargo test -p crypto_lib -- ed25519`

Expected: All 4 tests pass (3 existing + 1 new).

- [ ] **Step 6: Commit**

```bash
git add hogd/rust/crypto/src/ed25519.rs
git commit -m "fix: relax ed25519 debug assertions for null zero-length messages (#1612)

Allow message_ptr == null when message_len == 0 in ed25519_dalek_verify
and ed25519_dalek_sign, consistent with MessageSpan::to_slice() contract."
```

---

### Task 3: C++ integration regression test

**Files:**
- Modify: `hogd/hogdb/item_lock_test.cc`

- [ ] **Step 1: Write the C++ integration test**

Add this test to `hogd/hogdb/item_lock_test.cc`, immediately before the closing `} // namespace hogd` at line 766:

```cpp
TEST(ItemLockTest, EmptyRCQEntryCommitDoesNotCrash)
{
  RetrocausalQueueDelta delta;
  QueueEntry empty_entry;
  ASSERT_EQ(empty_entry.size(), 0U);
  ASSERT_EQ(empty_entry.data(), nullptr);
  delta.insert.added_entries.push_back(empty_entry);

  ItemChange change(RetrocausalQueueIt(std::move(delta)));

  ItemLock lock(0);
  auto ctx = make_rcq_context();
  auto lock_status = lock.try_atomic_change_lock(nullptr, &change, ctx);
  ASSERT_TRUE(lock_status.has_value());

  auto cost = lock.commit_change(change, ctx);
  EXPECT_GE(cost, 0);
}
```

- [ ] **Step 2: Build the test**

Run: `./scripts/build.sh -d ~/build/hogd-debug -t Debug -c gcc hogdb_test`

Expected: Compiles successfully.

- [ ] **Step 3: Run the test to verify it passes**

Run: `ctest --test-dir ~/build/hogd-debug --output-on-failure --timeout 120 -R "^ItemLockTest\\.EmptyRCQEntryCommitDoesNotCrash$"`

Expected: PASS — the Rust-side fix from Task 1 makes this safe.

- [ ] **Step 4: Run all existing item_lock tests to check for regressions**

Run: `ctest --test-dir ~/build/hogd-debug --output-on-failure --timeout 120 -R "^ItemLockTest\\."`

Expected: All tests pass.

- [ ] **Step 5: Commit**

```bash
git add hogd/hogdb/item_lock_test.cc
git commit -m "test: add regression test for empty RCQ entry commit (#1612)

Verifies that committing a zero-length QueueEntry through ItemLock
does not crash when hashing through Rust SipHash FFI."
```
