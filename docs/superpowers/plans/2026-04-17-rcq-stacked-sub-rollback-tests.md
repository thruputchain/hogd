# RCQ stacked-sub rollback tests implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend RCQ rollback-path regression coverage to stacked-subtransaction shapes, and document the two unreachable defensive undos inline.

**Architecture:** Four new `TEST_F`s appended to `direct_rcq_tests.cc` using the existing high-level `DirectRCQTests` fixture; two short NOTE comments added to `retrocausal_queue_lock.cc` at the unreachable-undo sites. No production logic changes. No new files.

**Tech Stack:** C++20/23, gtest, existing hogd CMake build.

**Branch:** `geoff/rcq-consumed-bit-rollback` (current). Build command: `cmake --build vscode --target direct_rpc_test -j8`. Test binary: `vscode/hogd/hogexec/runtime/direct_rpc_tests/direct_rpc_test`. Never run parallel builds; use a large Bash timeout (e.g. 600000 ms) for build commands.

**Spec:** `docs/superpowers/specs/2026-04-17-rcq-stacked-sub-rollback-tests-design.md`

---

## Task 1: StackedInsertThenInvalidConsumeInsertDoesNotPoison

Covers path A (consumed_ CAS undo) + path C (fetch_add undo) in a stacked sub where sub1 has no consume and sub2 adds a consume with invalid `max_fully_consumed`. Failure is `kInvalidExecutorBehavior`.

**Files:**
- Modify: `hogd/hogexec/runtime/direct_rpc_tests/direct_rcq_tests.cc` — append a new `TEST_F` at EOF.

- [ ] **Step 1.1: Add the test**

Append this test after the last existing `TEST_F` in `direct_rcq_tests.cc`, inside `namespace hogd`:

```cpp
// Stacked sub: sub1 = insert-only, sub2 = consume_insert with invalid
// max_fully_consumed. sub2 should fail at consume validation AFTER the CAS
// and fetch_add have run. ScopedUndo must roll back both so another tx in
// the same block can legitimately consume, and the per-block reservation
// only reflects sub1's inserts.
TEST_F(DirectRCQTests, StackedInsertThenInvalidConsumeInsertDoesNotPoison)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000'000);
  auto key =
      make_contract_data_addrkey(kContractAddress, "rcq_stacked_invalid");

  std::vector<QueueEntry> entries_100;
  entries_100.reserve(100);
  for (int i = 0; i < 100; ++i) {
    entries_100.push_back(
        make_rcq_entry({static_cast<uint8_t>(i & 0xFF),
                        static_cast<uint8_t>((i >> 8) & 0xFF)}));
  }
  std::vector<QueueEntry> entries_300;
  entries_300.reserve(300);
  for (int i = 0; i < 300; ++i) {
    entries_300.push_back(
        make_rcq_entry({static_cast<uint8_t>(i & 0xFF),
                        static_cast<uint8_t>((i >> 8) & 0xFF)}));
  }

  auto tx1 = make_new_per_tx_db_proxy();

  // Sub1: insert-only, 100 entries. reservation becomes 100.
  tx1.add_delta(key, make_rcq_insert_delta(entries_100),
                std::to_underlying(StorageDeltaFlags::None));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value()) << "sub1 insert must succeed";

  // Sub2: consume_insert cumulative with invalid max_fully_consumed=1 on a
  // fresh RCQ (max_live_block_num=0). Cumulative inserts = 300 (first 100
  // are sub1's, narrow's prefix check passes). Must fail at lock-time
  // consume validation.
  tx1.add_delta(key,
                make_rcq_consume_insert_delta(1, 0, {}, entries_300),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx1.try_push_current_subtransaction();
  ASSERT_FALSE(r.has_value())
      << "sub2 should fail with kInvalidExecutorBehavior (max_fully > max_live)";

  // Commit tx1. Sub2 never pushed, so only sub1's work persists.
  ASSERT_TRUE(tx1.commit_transaction({}).has_value());

  // A separate tx consumes one of sub1's current-block entries. Succeeds
  // iff sub2's CAS undo restored consumed_ to false.
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(key,
                make_rcq_consume_delta(0, 0, {entries_100[0]}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value())
      << "tx2 pure consume must succeed; sub2 must not leak consumed_";
  ASSERT_TRUE(tx2.commit_transaction({}).has_value());

  // A separate tx inserts 899 more entries. Succeeds iff the per-block
  // reservation is 100 (sub1 only). With a leaked fetch_add, reservation
  // would be 300 → 300+899>1000 → kConflict.
  std::vector<QueueEntry> entries_899;
  entries_899.reserve(899);
  for (int i = 0; i < 899; ++i) {
    entries_899.push_back(
        make_rcq_entry({static_cast<uint8_t>((i + 100) & 0xFF),
                        static_cast<uint8_t>(((i + 100) >> 8) & 0xFF)}));
  }
  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(key, make_rcq_insert_delta(entries_899),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx3.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value())
      << "tx3 insert 899 must succeed; sub2 must not leak fetch_add";
  ASSERT_TRUE(tx3.commit_transaction({}).has_value());
}
```

- [ ] **Step 1.2: Build**

Run `cmake --build vscode --target direct_rpc_test -j8` foreground, with a Bash timeout of 600000 ms. Do not run parallel builds.

- [ ] **Step 1.3: Run the new test**

```
vscode/hogd/hogexec/runtime/direct_rpc_tests/direct_rpc_test --gtest_filter=DirectRCQTests.StackedInsertThenInvalidConsumeInsertDoesNotPoison
```

Expected: PASS. This test is a regression test for the fix in commit `1ba3edfc6`; with the fix applied it passes, and if the fix were reverted it would fail at the tx2 or tx3 assertion.

If the test FAILS, report the exact failure output. Common causes:
- `tx1.commit_transaction` returns `!has_value()` after a failed sub push — switch to `tx1.abort_transaction()` and note it in the report (this changes the semantics — sub1's entries will not be visible to tx2, so the test must be restructured: use a separate tx_setup to commit entry `X` first, and consume `X` in tx2 instead of an entry_100 entry).
- narrow rejects sub2 — check the prefix carefully (sub2's first 100 entries must equal entries_100).

- [ ] **Step 1.4: Commit**

```bash
git add hogd/hogexec/runtime/direct_rpc_tests/direct_rcq_tests.cc
git commit -m "$(cat <<'EOF'
test: stacked sub invalid-consume rollback does not poison lock

Regression test for #1489 fix in the stacked-sub shape: sub1 insert-only,
sub2 adds consume_insert with invalid max_fully_consumed. Verifies CAS
and fetch_add undos fire.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: StackedInsertThenOverfullConsumeInsertDoesNotPoison

Covers path A + path C via the **overflow** (kConflict) failure leg in the same stacked shape as Task 1. Verifies the guard unwinds on overflow-return, not just kInvalidExecutorBehavior.

**Files:**
- Modify: `hogd/hogexec/runtime/direct_rpc_tests/direct_rcq_tests.cc` — append.

- [ ] **Step 2.1: Add the test**

Append after Task 1's test:

```cpp
// Stacked sub: sub1 insert-only, sub2 consume_insert VALID but the
// cumulative insert count pushes the per-block reservation past the cap.
// Different failure leg than StackedInsertThenInvalidConsumeInsert: this
// returns kConflict from the overflow check, not kInvalidExecutorBehavior
// from consume validation. ScopedUndo must still unwind CAS + fetch_add.
TEST_F(DirectRCQTests, StackedInsertThenOverfullConsumeInsertDoesNotPoison)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000'000);
  auto key =
      make_contract_data_addrkey(kContractAddress, "rcq_stacked_overfull");

  // Filler tx: 800 inserts. Reservation becomes 800.
  std::vector<QueueEntry> entries_filler;
  entries_filler.reserve(800);
  for (int i = 0; i < 800; ++i) {
    entries_filler.push_back(
        make_rcq_entry({static_cast<uint8_t>((i + 1000) & 0xFF),
                        static_cast<uint8_t>(((i + 1000) >> 8) & 0xFF)}));
  }
  auto tx_filler = make_new_per_tx_db_proxy();
  tx_filler.add_delta(key, make_rcq_insert_delta(entries_filler),
                      std::to_underlying(StorageDeltaFlags::None));
  auto r = tx_filler.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx_filler.commit_transaction({}).has_value());

  std::vector<QueueEntry> entries_100;
  entries_100.reserve(100);
  for (int i = 0; i < 100; ++i) {
    entries_100.push_back(
        make_rcq_entry({static_cast<uint8_t>(i & 0xFF),
                        static_cast<uint8_t>((i >> 8) & 0xFF)}));
  }
  std::vector<QueueEntry> entries_300;
  entries_300.reserve(300);
  for (int i = 0; i < 300; ++i) {
    entries_300.push_back(
        make_rcq_entry({static_cast<uint8_t>(i & 0xFF),
                        static_cast<uint8_t>((i >> 8) & 0xFF)}));
  }

  auto tx1 = make_new_per_tx_db_proxy();

  // Sub1: 100 inserts. reservation: 800 -> 900.
  tx1.add_delta(key, make_rcq_insert_delta(entries_100),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value()) << "sub1 insert must succeed";

  // Sub2: valid consume + 300 cumulative inserts. additional = 200.
  // fetch_add would push reservation to 1100 > 1000 cap. Overflow returns
  // kConflict (r.has_value() && !r.value() pattern).
  tx1.add_delta(key,
                make_rcq_consume_insert_delta(0, 0, {}, entries_300),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && !r.value())
      << "sub2 should fail with kConflict (800+100+200 > 1000)";

  ASSERT_TRUE(tx1.commit_transaction({}).has_value());

  // Verify consumed_ was restored: another tx can consume a current-block
  // entry from sub1. With a leaked CAS, this would return kConflict.
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(key,
                make_rcq_consume_delta(0, 0, {entries_100[0]}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value())
      << "tx2 pure consume must succeed; sub2 must not leak consumed_";
  ASSERT_TRUE(tx2.commit_transaction({}).has_value());
}
```

- [ ] **Step 2.2: Build**

Run `cmake --build vscode --target direct_rpc_test -j8` foreground, Bash timeout 600000 ms.

- [ ] **Step 2.3: Run the test**

```
vscode/hogd/hogexec/runtime/direct_rpc_tests/direct_rpc_test --gtest_filter=DirectRCQTests.StackedInsertThenOverfullConsumeInsertDoesNotPoison
```

Expected: PASS. If the test FAILS, report the failure.

- [ ] **Step 2.4: Commit**

```bash
git add hogd/hogexec/runtime/direct_rpc_tests/direct_rcq_tests.cc
git commit -m "$(cat <<'EOF'
test: stacked sub overfull consume_insert rollback does not poison lock

Regression test covering the overflow (kConflict) failure leg of
ScopedUndo: sub1 insert-only, sub2 adds consume_insert whose cumulative
reservation exceeds the per-block cap.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: StackedConsumeInsertThenInvalidConsumeInsertDoesNotPoison

Covers path C (fetch_add undo) in the `this_tx_already_consumed=true` shape — the branch where no new CAS runs in sub2 because sub1 already set `consumed_`. Only the reservation undo fires.

**Files:**
- Modify: `hogd/hogexec/runtime/direct_rpc_tests/direct_rcq_tests.cc` — append.

- [ ] **Step 3.1: Add the test**

Append after Task 2's test:

```cpp
// Stacked sub: sub1 = valid consume_insert (sets consumed_ via CAS),
// sub2 = consume_insert with invalid max_fully_consumed. Because
// this_tx_already_consumed is true in sub2, no CAS runs — only the
// fetch_add undo is in play. consumed_ stays set (sub1 owns it); the
// reservation must reflect only sub1's inserts after sub2 fails.
TEST_F(DirectRCQTests,
       StackedConsumeInsertThenInvalidConsumeInsertDoesNotPoison)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000'000);
  auto key =
      make_contract_data_addrkey(kContractAddress, "rcq_stacked_already");

  std::vector<QueueEntry> entries_100;
  entries_100.reserve(100);
  for (int i = 0; i < 100; ++i) {
    entries_100.push_back(
        make_rcq_entry({static_cast<uint8_t>(i & 0xFF),
                        static_cast<uint8_t>((i >> 8) & 0xFF)}));
  }
  std::vector<QueueEntry> entries_300;
  entries_300.reserve(300);
  for (int i = 0; i < 300; ++i) {
    entries_300.push_back(
        make_rcq_entry({static_cast<uint8_t>(i & 0xFF),
                        static_cast<uint8_t>((i >> 8) & 0xFF)}));
  }

  auto tx1 = make_new_per_tx_db_proxy();

  // Sub1: valid consume_insert, 100 inserts. consumed_=true, reservation=100.
  tx1.add_delta(key,
                make_rcq_consume_insert_delta(0, 0, {}, entries_100),
                std::to_underlying(StorageDeltaFlags::None));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value()) << "sub1 consume_insert must succeed";

  // Sub2: invalid max_fully_consumed=1, cumulative 300 inserts. narrow
  // passes (consume is still present, monotonic; inserts grew). In the
  // lock: this_tx_already_consumed=true so no new CAS. fetch_add(200)
  // runs, undo pushed. Consume validation trips kInvalidExecutorBehavior.
  // Only the reservation undo fires.
  tx1.add_delta(key,
                make_rcq_consume_insert_delta(1, 0, {}, entries_300),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx1.try_push_current_subtransaction();
  ASSERT_FALSE(r.has_value())
      << "sub2 should fail with kInvalidExecutorBehavior";

  ASSERT_TRUE(tx1.commit_transaction({}).has_value());

  // Reservation should be 100 (sub1 only) if fetch_add was undone; with a
  // leak it would be 300. Verify by inserting 899 more entries from a
  // separate tx: 100+899=999 <= 1000 passes; 300+899=1199 > 1000 fails.
  std::vector<QueueEntry> entries_899;
  entries_899.reserve(899);
  for (int i = 0; i < 899; ++i) {
    entries_899.push_back(
        make_rcq_entry({static_cast<uint8_t>((i + 100) & 0xFF),
                        static_cast<uint8_t>(((i + 100) >> 8) & 0xFF)}));
  }
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(key, make_rcq_insert_delta(entries_899),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value())
      << "tx2 insert 899 must succeed; sub2 must not leak fetch_add";
  ASSERT_TRUE(tx2.commit_transaction({}).has_value());
}
```

- [ ] **Step 3.2: Build**

Run `cmake --build vscode --target direct_rpc_test -j8` foreground, Bash timeout 600000 ms.

- [ ] **Step 3.3: Run the test**

```
vscode/hogd/hogexec/runtime/direct_rpc_tests/direct_rpc_test --gtest_filter=DirectRCQTests.StackedConsumeInsertThenInvalidConsumeInsertDoesNotPoison
```

Expected: PASS.

- [ ] **Step 3.4: Commit**

```bash
git add hogd/hogexec/runtime/direct_rpc_tests/direct_rcq_tests.cc
git commit -m "$(cat <<'EOF'
test: stacked consume+invalid-consume rollback does not leak reservation

Regression test for the this_tx_already_consumed=true branch of
ScopedUndo: only the fetch_add undo is in play; consumed_ is unchanged.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: PopConsumeAddingSubReleasesConsumerBit

Happy-path coverage of the else-if CLEAR (the clear itself runs; its undo does not — documented as defensive-only in Task 5).

**Files:**
- Modify: `hogd/hogexec/runtime/direct_rpc_tests/direct_rcq_tests.cc` — append.

- [ ] **Step 4.1: Add the test**

Append after Task 3's test:

```cpp
// Pop of a consume-adding sub: sub1 = insert-only, sub2 = consume_insert
// that adds the consume. Pop sub2 → the lock call has before.consume set
// and after.consume null, triggering the else-if CLEAR in
// try_atomic_change_lock. This is the reachable half of that branch;
// its undo path is unreachable today (documented inline).
TEST_F(DirectRCQTests, PopConsumeAddingSubReleasesConsumerBit)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key =
      make_contract_data_addrkey(kContractAddress, "rcq_pop_adds_consume");

  auto tx1 = make_new_per_tx_db_proxy();

  // Sub1: insert-only, one entry A.
  tx1.add_delta(key, make_rcq_insert_delta({make_rcq_entry({0})}),
                std::to_underlying(StorageDeltaFlags::None));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value()) << "sub1 insert must succeed";

  // Sub2: consume_insert cumulative. Adds a consume + one more insert so
  // that sub2's cumulative inserts (2) are a superset of sub1's (1) and
  // the prefix check at lock line ~160 passes. Consume is valid (max_fully
  // = 0 on a fresh RCQ). consumed_ becomes true.
  tx1.add_delta(key,
                make_rcq_consume_insert_delta(
                    0, 0, {}, {make_rcq_entry({0}), make_rcq_entry({1})}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value()) << "sub2 consume_insert must succeed";

  // Pop sub2: lock call sees before.consume != null, after.consume null,
  // this_tx_already_consumed = true. else-if CLEAR stores consumed_ =
  // false. Shrink branch fetch_sub(1). Final success → undo disarm.
  auto pop_r = tx1.pop_subtransactions(1);
  ASSERT_TRUE(pop_r.has_value()) << "pop_subtransactions must succeed";

  ASSERT_TRUE(tx1.commit_transaction({}).has_value());

  // A separate tx consumes entry A from current block. With the else-if
  // CLEAR working, consumed_ was released and the CAS here succeeds.
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(key,
                make_rcq_consume_delta(0, 0, {make_rcq_entry({0})}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value())
      << "tx2 pure consume must succeed; else-if CLEAR must have released "
         "consumed_";
  ASSERT_TRUE(tx2.commit_transaction({}).has_value());
}
```

- [ ] **Step 4.2: Build**

Run `cmake --build vscode --target direct_rpc_test -j8` foreground, Bash timeout 600000 ms.

- [ ] **Step 4.3: Run the test**

```
vscode/hogd/hogexec/runtime/direct_rpc_tests/direct_rpc_test --gtest_filter=DirectRCQTests.PopConsumeAddingSubReleasesConsumerBit
```

Expected: PASS.

- [ ] **Step 4.4: Commit**

```bash
git add hogd/hogexec/runtime/direct_rpc_tests/direct_rcq_tests.cc
git commit -m "$(cat <<'EOF'
test: pop of consume-adding sub releases consumer bit

Positive-path coverage of try_atomic_change_lock's else-if CLEAR branch.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Defensive-undo comments in `retrocausal_queue_lock.cc`

Document the two undos whose failure path is unreachable today, so a future editor understands the invariant they preserve.

**Files:**
- Modify: `hogd/hogdb/item_lock/retrocausal_queue_lock.cc` — two comment insertions.

- [ ] **Step 5.1: Add the else-if branch comment**

Open `hogd/hogdb/item_lock/retrocausal_queue_lock.cc`. Locate the else-if branch that starts around line 186. The existing code is:

```cpp
  else if (this_tx_already_consumed) {
    // Update removed the consume.
    consumed_.store(false, std::memory_order_relaxed);
    undo.push([this] { consumed_.store(true, std::memory_order_relaxed); });
  }
```

Replace it with:

```cpp
  else if (this_tx_already_consumed) {
    // Update removed the consume.
    consumed_.store(false, std::memory_order_relaxed);
    // NOTE: this undo is defensive. Today this branch is only reachable via
    // pop_subtransactions, whose lock call has new_entries <=
    // previous_entries (shrink) and thus never hits the overflow check -
    // no failure path exists downstream. RetrocausalQueueIt::narrow() in
    // item_change.cc blocks forward drops. The undo preserves the
    // "ScopedUndo owns rollback" invariant so any future edit that adds a
    // fallible step after this point stays correct automatically.
    undo.push([this] { consumed_.store(true, std::memory_order_relaxed); });
  }
```

- [ ] **Step 5.2: Add the shrink branch comment**

Locate the shrink branch around line 229. The existing code is:

```cpp
  else if (new_entries_count < previous_entries_count) {
    // Rollback case: unreserve entries that are no longer needed.
    const size_t entries_to_unreserve =
        previous_entries_count - new_entries_count;
    reserved_entry_count_.fetch_sub(entries_to_unreserve,
                                    std::memory_order_relaxed);
    undo.push([this, entries_to_unreserve] {
      reserved_entry_count_.fetch_add(entries_to_unreserve,
                                      std::memory_order_relaxed);
    });
  }
```

Replace it with:

```cpp
  else if (new_entries_count < previous_entries_count) {
    // Rollback case: unreserve entries that are no longer needed.
    const size_t entries_to_unreserve =
        previous_entries_count - new_entries_count;
    reserved_entry_count_.fetch_sub(entries_to_unreserve,
                                    std::memory_order_relaxed);
    // NOTE: same defensive-undo rationale as the else-if consumed_ branch
    // above. The only shrink caller today is pop_subtransactions, which
    // cannot fail downstream. Kept to preserve the ScopedUndo "owns
    // rollback" invariant across future edits.
    undo.push([this, entries_to_unreserve] {
      reserved_entry_count_.fetch_add(entries_to_unreserve,
                                      std::memory_order_relaxed);
    });
  }
```

- [ ] **Step 5.3: Build**

Run `cmake --build vscode --target direct_rpc_test -j8` foreground, Bash timeout 600000 ms. Comments only, must build without warnings.

- [ ] **Step 5.4: Commit**

```bash
git add hogd/hogdb/item_lock/retrocausal_queue_lock.cc
git commit -m "$(cat <<'EOF'
docs(hogdb): annotate defensive RCQ rollback undos

Document the two ScopedUndo registrations whose failure paths are
unreachable today (blocked by narrow() and pop's shrink-only call shape)
but are retained to preserve the "owns rollback" invariant across
future edits.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: Regression sweep + push

- [ ] **Step 6.1: Run the full DirectRCQTests suite**

```
vscode/hogd/hogexec/runtime/direct_rpc_tests/direct_rpc_test --gtest_filter='DirectRCQTests.*'
```

Expected: all tests PASS (should now be 87 with the four additions, all passing, no skips within DirectRCQTests).

- [ ] **Step 6.2: Run the full binary for broader safety**

```
vscode/hogd/hogexec/runtime/direct_rpc_tests/direct_rpc_test
```

Expected: 249 total tests, 228 passed, 21 skipped (pre-existing conditional skips), 0 failed. The skipped list should match the one observed earlier in this branch.

- [ ] **Step 6.3: If any regression, diagnose**

If a previously-passing test now fails, the new tests are interacting with shared state or the comment edit altered something unintended. Check:
- Key uniqueness: each new test uses a distinct key string — verify no collision with an existing test's key.
- The comment-edit diff in `retrocausal_queue_lock.cc` must touch comments only — no code changes.

Fix any regression and commit.

- [ ] **Step 6.4: Push**

```bash
git push
```

- [ ] **Step 6.5: Monitor CI**

Poll `gh pr checks 1491` until no pending checks. Fix any new failures. The `tidy` check is the most likely to flag — the comments are plain text with no code changes, so no new warnings expected, but verify.

---

## Self-review

**Spec coverage:**

| Spec requirement | Task |
|---|---|
| Test 1 (StackedInsertThenInvalidConsumeInsert) | 1 |
| Test 2 (StackedInsertThenOverfullConsumeInsert) | 2 |
| Test 3 (StackedConsumeInsertThenInvalidConsumeInsert) | 3 |
| Test 4 (PopConsumeAddingSubReleasesConsumerBit) | 4 |
| else-if defensive-undo comment | 5.1 |
| shrink defensive-undo comment | 5.2 |
| Regression sweep | 6 |
| Indirect observability via follow-up txs | 1 (tx2+tx3), 2 (tx2), 3 (tx2), 4 (tx2) |

Every spec item maps to a task.

**Placeholder scan:** No TBDs, TODOs, or "similar to…" shortcuts. Every code block is final content.

**Type/name consistency:** All four test names match the spec. Helper functions used (`make_rcq_insert_delta`, `make_rcq_consume_insert_delta`, `make_rcq_consume_delta`, `make_rcq_entry`, `make_new_per_tx_db_proxy`, `setup_grant_storage_token`, `make_contract_data_addrkey`) are the same ones used throughout the existing `direct_rcq_tests.cc`. `pop_subtransactions(1)` matches `StressPopSubtransactionWithConsumeDelta`'s usage.
