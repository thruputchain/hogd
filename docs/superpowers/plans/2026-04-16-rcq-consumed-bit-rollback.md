# RCQ consumed_ bit rollback implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `RetrocausalQueueLock::try_atomic_change_lock` rollback-symmetric for `consumed_` and `reserved_entry_count_` across every fallible early-return path, closing issue #1489 and its siblings.

**Architecture:** Introduce a function-local RAII `ScopedUndo` that holds a list of undo closures, runs them on destruction unless `disarm()`ed, and has deleted copy/move. Register an undo right after each successful mutation of the two atomic fields. On the single success path, `disarm()`. Keep mutation logic inline (no refactor).

**Tech Stack:** C++20/23, gtest, hogd CMake build.

**Branch:** `geoff/rcq-consumed-bit-rollback` (already created)

**Spec:** `docs/superpowers/specs/2026-04-16-rcq-consumed-bit-rollback-design.md`

---

## Task 1: Reproduce the documented bug with a failing test (PoC from #1489)

**Files:**
- Modify: `hogd/hogexec/runtime/direct_rpc_tests/direct_rcq_tests.cc` — append a new `TEST_F` at the end of the file.

- [ ] **Step 1.1: Add the failing test**

Append this test to the end of `hogd/hogexec/runtime/direct_rpc_tests/direct_rcq_tests.cc` (before the trailing closing `}` of the `hogd` namespace if the file wraps tests in a namespace — check the existing tail of the file first; tests in this file are not namespaced, so just append before EOF).

```cpp
// H005 (#1489): An overfull consume+insert transaction must not poison the
// single-consumer bit. A later legitimate consume on the same key in the same
// block must still succeed.
TEST_F(DirectRCQTests, OverfullConsumeInsertDoesNotPoisonConsumerBit)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000'000);
  auto key = make_contract_data_addrkey(kContractAddress, "rcq_h005_poison");

  // TX1: Insert 900 entries to fill most of the per-block budget (max=1000).
  std::vector<QueueEntry> entries_900;
  entries_900.reserve(900);
  for (int i = 0; i < 900; ++i) {
    entries_900.push_back(make_rcq_entry(
        {static_cast<uint8_t>(i & 0xFF),
         static_cast<uint8_t>((i >> 8) & 0xFF)}));
  }
  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(key, make_rcq_insert_delta(entries_900),
                std::to_underlying(StorageDeltaFlags::None));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value()) << "TX1 insert 900 should succeed";
  ASSERT_TRUE(tx1.commit_transaction({}).has_value());

  // TX2: Consume + insert 200 entries. Aggregate reservation 900 + 200 = 1100
  // exceeds the 1000-entry per-block cap, so push must fail with kConflict.
  std::vector<QueueEntry> entries_200;
  entries_200.reserve(200);
  for (int i = 0; i < 200; ++i) {
    entries_200.push_back(make_rcq_entry(
        {static_cast<uint8_t>((i + 100) & 0xFF),
         static_cast<uint8_t>(((i + 100) >> 8) & 0xFF)}));
  }
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(
      key,
      make_rcq_consume_insert_delta(0, 0, {entries_900[0]}, entries_200),
      std::to_underlying(StorageDeltaFlags::None));
  r = tx2.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && !r.value())
      << "TX2 consume+insert 200 should fail (aggregate > 1000)";
  ASSERT_TRUE(tx2.commit_transaction({}).has_value());

  // TX3: legitimate pure consume. TX2's failed reservation must not leak the
  // consumed_ bit, so TX3 must succeed.
  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(key, make_rcq_consume_delta(0, 0, {entries_900[0]}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx3.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value())
      << "TX3 pure consume must succeed after TX2's failed overfull "
         "reservation";
  ASSERT_TRUE(tx3.commit_transaction({}).has_value());
}
```

- [ ] **Step 1.2: Build the test binary using the build skill**

Invoke the `build` skill with target `direct_rpc_test`. Do not run parallel builds (per user memory — sequential only).

- [ ] **Step 1.3: Run the new test and confirm it fails with the expected symptom**

Run:

```bash
~/build/hogd-debug/hogd/hogexec/runtime/direct_rpc_tests/direct_rpc_test \
  --gtest_filter=DirectRCQTests.OverfullConsumeInsertDoesNotPoisonConsumerBit
```

Expected: FAIL on the TX3 assertion (`TX3 pure consume must succeed…`). `r.has_value()` is true, `r.value()` is false — TX3 got `kConflict` because `consumed_` is still set. This reproduces #1489.

- [ ] **Step 1.4: Commit the failing test**

```bash
git add hogd/hogexec/runtime/direct_rpc_tests/direct_rcq_tests.cc
git commit -m "$(cat <<'EOF'
test: reproduce #1489 overfull RCQ consume+insert poisons consumer bit

Failing test pinned to current (buggy) behavior. Will flip to passing
once try_atomic_change_lock is made rollback-symmetric.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Add failing test for sibling `consumed_` leak on `kInvalidExecutorBehavior`

**Files:**
- Modify: `hogd/hogexec/runtime/direct_rpc_tests/direct_rcq_tests.cc` — append.

Context: In `try_atomic_change_lock`, the consume-state validation block (lines 182-267) has several `kInvalidExecutorBehavior` returns after the `consumed_` CAS has already set the flag. We trigger line 197 (`new_consume.max_fully_consumed_block_number > meta.max_live_block_num`), which is the simplest path on a fresh RCQ. With the bug, `consumed_` leaks; with the fix, the guard clears it.

- [ ] **Step 2.1: Add the failing test**

Append to `direct_rcq_tests.cc`:

```cpp
// Sibling leak: a consume that trips kInvalidExecutorBehavior after the
// consumed_ CAS must not leak the single-consumer bit to a subsequent
// legitimate consume in the same block.
TEST_F(DirectRCQTests,
       InvalidExecutorConsumeDoesNotPoisonConsumerBit)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000);
  auto key =
      make_contract_data_addrkey(kContractAddress, "rcq_invalid_leak_cbit");

  // TX1: insert one entry so TX3 has something to consume.
  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(key, make_rcq_insert_delta({make_rcq_entry({0})}),
                std::to_underlying(StorageDeltaFlags::None));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx1.commit_transaction({}).has_value());

  // TX2: consume with max_fully_consumed_block_number = 1 on a fresh RCQ
  // (max_live_block_num = 0). This trips
  // retrocausal_queue_lock.cc line ~197 and returns kInvalidExecutorBehavior
  // AFTER consumed_ was already CASed to true.
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(key, make_rcq_consume_delta(1, 0, {}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx2.try_push_current_subtransaction();
  // kInvalidExecutorBehavior surfaces as !r.has_value(), distinct from kConflict.
  ASSERT_FALSE(r.has_value())
      << "TX2 should hit kInvalidExecutorBehavior (past block > max_live)";
  ASSERT_TRUE(tx2.abort_transaction().has_value());

  // TX3: legitimate pure consume of the current-block entry. Should succeed
  // once consumed_ is rolled back by the guard.
  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(key, make_rcq_consume_delta(0, 0, {make_rcq_entry({0})}),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx3.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value())
      << "TX3 pure consume must succeed after TX2's kInvalidExecutorBehavior";
  ASSERT_TRUE(tx3.commit_transaction({}).has_value());
}
```

- [ ] **Step 2.2: Build and run the new test**

Invoke the build skill (target `direct_rpc_test`), then:

```bash
~/build/hogd-debug/hogd/hogexec/runtime/direct_rpc_tests/direct_rpc_test \
  --gtest_filter=DirectRCQTests.InvalidExecutorConsumeDoesNotPoisonConsumerBit
```

Expected outcomes:
- TX2 `ASSERT_FALSE(r.has_value())` — passes (current behavior already returns error).
- TX3 `ASSERT_TRUE(r.has_value() && r.value())` — **fails**, because `consumed_` is leaked.

**If TX2's assertion fails** (`r.has_value()` is true), that means the path didn't reach line 197; it was short-circuited elsewhere (e.g. by narrow() at item_change.cc). In that case, switch the trigger: pass `max_fully=0, partial=1, current_block={}` which trips line 211 (`partial_consumed_number > 0` when all blocks are fully consumed on a fresh RCQ). Re-run and re-check.

- [ ] **Step 2.3: Commit the failing test**

```bash
git add hogd/hogexec/runtime/direct_rpc_tests/direct_rcq_tests.cc
git commit -m "$(cat <<'EOF'
test: reproduce sibling consumed_ leak on kInvalidExecutorBehavior

Failing test pinned to current (buggy) behavior. Complements #1489 test.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Add failing test for `reserved_entry_count_` leak on `kInvalidExecutorBehavior`

**Files:**
- Modify: `hogd/hogexec/runtime/direct_rpc_tests/direct_rcq_tests.cc` — append.

Context: On the `delta.consume` path, the entry-count `fetch_add` at line 164 runs **before** the `kInvalidExecutorBehavior` checks at 197-234. If those checks fail, the reservation leaks, so a subsequent honest insert can hit the 1000-entry cap despite being under budget.

- [ ] **Step 3.1: Add the failing test**

Append to `direct_rcq_tests.cc`:

```cpp
// Sibling leak: a consume+insert that trips kInvalidExecutorBehavior after
// the entry-count fetch_add must not leak the reservation.
TEST_F(DirectRCQTests,
       InvalidExecutorConsumeInsertDoesNotPoisonEntryReservation)
{
  setup_grant_storage_token(kContractAddress, 1'000'000'000'000);
  auto key =
      make_contract_data_addrkey(kContractAddress, "rcq_invalid_leak_res");

  // TX1: reserve 500 entries.
  std::vector<QueueEntry> entries_500;
  entries_500.reserve(500);
  for (int i = 0; i < 500; ++i) {
    entries_500.push_back(make_rcq_entry(
        {static_cast<uint8_t>(i & 0xFF),
         static_cast<uint8_t>((i >> 8) & 0xFF)}));
  }
  auto tx1 = make_new_per_tx_db_proxy();
  tx1.add_delta(key, make_rcq_insert_delta(entries_500),
                std::to_underlying(StorageDeltaFlags::None));
  auto r = tx1.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value());
  ASSERT_TRUE(tx1.commit_transaction({}).has_value());

  // TX2: consume_insert with 400 entries + invalid consume. Reservation
  // 500 + 400 = 900 passes the 1000 cap, then kInvalidExecutorBehavior
  // fires in the consume-validation block, leaking the 400-entry reservation.
  std::vector<QueueEntry> entries_400;
  entries_400.reserve(400);
  for (int i = 0; i < 400; ++i) {
    entries_400.push_back(make_rcq_entry(
        {static_cast<uint8_t>((i + 200) & 0xFF),
         static_cast<uint8_t>(((i + 200) >> 8) & 0xFF)}));
  }
  auto tx2 = make_new_per_tx_db_proxy();
  tx2.add_delta(
      key,
      make_rcq_consume_insert_delta(1, 0, {}, entries_400),
      std::to_underlying(StorageDeltaFlags::None));
  r = tx2.try_push_current_subtransaction();
  ASSERT_FALSE(r.has_value())
      << "TX2 should hit kInvalidExecutorBehavior (past block > max_live)";
  ASSERT_TRUE(tx2.abort_transaction().has_value());

  // TX3: legitimate insert of 200 entries. Total 500 + 200 = 700 <= 1000,
  // must succeed. With the leak it becomes 500 + 400 + 200 = 1100 and fails.
  std::vector<QueueEntry> entries_200;
  entries_200.reserve(200);
  for (int i = 0; i < 200; ++i) {
    entries_200.push_back(make_rcq_entry(
        {static_cast<uint8_t>((i + 700) & 0xFF),
         static_cast<uint8_t>(((i + 700) >> 8) & 0xFF)}));
  }
  auto tx3 = make_new_per_tx_db_proxy();
  tx3.add_delta(key, make_rcq_insert_delta(entries_200),
                std::to_underlying(StorageDeltaFlags::None));
  r = tx3.try_push_current_subtransaction();
  ASSERT_TRUE(r.has_value() && r.value())
      << "TX3 insert 200 must succeed after TX2's reservation is rolled back";
  ASSERT_TRUE(tx3.commit_transaction({}).has_value());
}
```

- [ ] **Step 3.2: Build and run the test**

Invoke build skill (target `direct_rpc_test`), then:

```bash
~/build/hogd-debug/hogd/hogexec/runtime/direct_rpc_tests/direct_rpc_test \
  --gtest_filter=DirectRCQTests.InvalidExecutorConsumeInsertDoesNotPoisonEntryReservation
```

Expected: TX2 assertion passes (returns error), TX3 assertion **fails** (gets `kConflict` from cap check because the 400-entry reservation leaked). If TX2's trigger path is short-circuited, fall back to the `partial=1` trigger as noted in Task 2.

- [ ] **Step 3.3: Commit the failing test**

```bash
git add hogd/hogexec/runtime/direct_rpc_tests/direct_rcq_tests.cc
git commit -m "$(cat <<'EOF'
test: reproduce sibling entry-reservation leak on kInvalidExecutorBehavior

Failing test for the second leak sibling to #1489.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Implement `ScopedUndo` and wire rollbacks into `try_atomic_change_lock`

**Files:**
- Modify: `hogd/hogdb/item_lock/retrocausal_queue_lock.cc`

- [ ] **Step 4.1: Add the `<functional>` include if not already present**

Open `hogd/hogdb/item_lock/retrocausal_queue_lock.cc`. The current includes are:

```cpp
#include <algorithm>
#include <atomic>
#include <cassert>
#include <expected>
#include <mutex>
#include <optional>
#include <set>
#include <utility>
#include <vector>
```

Add `#include <functional>` after `#include <expected>` (kept sorted).

- [ ] **Step 4.2: Define `ScopedUndo` in an anonymous namespace inside the file**

Insert right after the existing `namespace hogd {` line (so the type is file-local, not exported):

```cpp
namespace {

// RAII rollback helper local to try_atomic_change_lock. Holds a list of undo
// closures that run on destruction (in reverse order of registration) unless
// disarm() has been called. Not copyable, not movable: instances are strictly
// function-local and should not escape.
class ScopedUndo {
 public:
  ScopedUndo() = default;
  ~ScopedUndo()
  {
    if (!armed_) {
      return;
    }
    // Run in reverse, mirroring stack-unwind semantics.
    for (auto it = actions_.rbegin(); it != actions_.rend(); ++it) {
      (*it)();
    }
  }

  ScopedUndo(const ScopedUndo&) = delete;
  ScopedUndo& operator=(const ScopedUndo&) = delete;
  ScopedUndo(ScopedUndo&&) = delete;
  ScopedUndo& operator=(ScopedUndo&&) = delete;

  void push(std::function<void()> action)
  {
    actions_.push_back(std::move(action));
  }

  void disarm() { armed_ = false; }

 private:
  std::vector<std::function<void()>> actions_;
  bool armed_ = true;
};

} // namespace
```

- [ ] **Step 4.3: Register undos and disarm on success in `try_atomic_change_lock`**

Locate `RetrocausalQueueLock::try_atomic_change_lock` in the file. Modify only the `after != nullptr` branch (starting at line 97). Do NOT touch the `after == nullptr` branch at lines 73-94 — it is the committed-delta undo path and already correct.

Insert a `ScopedUndo undo;` declaration at the top of the `after != nullptr` branch, immediately after the `CHECK(after != nullptr);` at line 97. Then register undos at each mutation point and disarm at the end.

Replace the section from line 117 through line 180 (the consumed_/reserved_entry_count_ manipulation). Concretely:

**At the successful `consumed_` CAS (line 130-134):** after the CAS succeeds, register an undo that stores `false` back. Also register an undo for the explicit `consumed_.store(false)` at line 139 (the `else if (this_tx_already_consumed)` branch), which should re-store `true` on failure.

**At the `fetch_add` (line 164):** after the overflow check passes, register an undo that `fetch_sub`s `additional_entries`. On overflow, remove the inline `fetch_sub` at 169-170 and just `return HogDBError{kConflict}` — the guard cleans up both the reservation **and** `consumed_`.

**At the `fetch_sub` in the shrink branch (line 178):** register a matching `fetch_add` undo.

**Before the final success return:** call `undo.disarm();`.

The full replacement for lines 117-180 is:

```cpp
  // Check strict single-consumer constraint
  const bool this_tx_already_consumed =
      (before != nullptr && before->delta().consume != nullptr);
  if (delta.consume) {
    if (consumed_.load(std::memory_order_relaxed) &&
        !this_tx_already_consumed) {
      // We must abort to maintain MPSC semantics. If this tx has not consumed
      // any entries before, then some other transaction set delta.consume.
      // Hence, there would be two consumers for the block, which is a
      // conflict.
      return HogDBError{HogDBCode::kConflict};
    }

    if (!this_tx_already_consumed) {
      bool expected = false;
      if (!consumed_.compare_exchange_strong(expected, true)) {
        return HogDBError{HogDBCode::kConflict};
      }
      undo.push([this] {
        consumed_.store(false, std::memory_order_relaxed);
      });
    }
  }
  else if (this_tx_already_consumed) {
    // Update removed the consume.
    consumed_.store(false, std::memory_order_relaxed);
    undo.push([this] {
      consumed_.store(true, std::memory_order_relaxed);
    });
  }

  auto r = get_meta(&ctx);
  if (!r.has_value()) {
    return r.error();
  }
  CHECK(*r != nullptr);
  auto &meta = **r;

  // Executor should ensure that we never consume from a block that hasn't been
  // inserted to yet.
  CHECK(meta.max_live_block_num >=
        meta.consume_state.max_fully_consumed_block_number);

  // Reserve space in the RCQ block that will be generated. If there are too
  // many inserts in the same block, abort.
  const size_t new_entries_count = delta.insert.added_entries.size();
  const size_t previous_entries_count =
      (before != nullptr) ? before->delta().insert.added_entries.size() : 0;

  if (new_entries_count > previous_entries_count) {
    const size_t additional_entries =
        new_entries_count - previous_entries_count;
    const size_t reserved_before =
        reserved_entry_count_.fetch_add(additional_entries,
                                        std::memory_order_relaxed);

    if (reserved_before + additional_entries >
        kRetrocausalQueueBlockEntriesMaxLen) {
      // Don't unreserve inline — register the undo and let ScopedUndo roll
      // back both the reservation and consumed_ on return.
      undo.push([this, additional_entries] {
        reserved_entry_count_.fetch_sub(additional_entries,
                                        std::memory_order_relaxed);
      });
      return HogDBError{HogDBCode::kConflict};
    }
    undo.push([this, additional_entries] {
      reserved_entry_count_.fetch_sub(additional_entries,
                                      std::memory_order_relaxed);
    });
  }
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

Then, at the very end of the `after != nullptr` branch, immediately before the final `return standalone_cost + ctx.item_lock_snapshot_cost;`, insert:

```cpp
  undo.disarm();
```

Note: insert the `ScopedUndo undo;` declaration right after `CHECK(after != nullptr);` (currently line 97), above the `auto const &delta = after->delta();` line. It must be declared before any mutation so destructor order correctly runs undos before the function returns.

- [ ] **Step 4.4: Verify the edit compiles**

Invoke build skill (target `direct_rpc_test`). If a compile error surfaces, fix it and re-run. Common fix: forgotten `std::function` include, or a typo in the lambda capture list.

- [ ] **Step 4.5: Re-run the three new tests and confirm they now pass**

```bash
~/build/hogd-debug/hogd/hogexec/runtime/direct_rpc_tests/direct_rpc_test \
  --gtest_filter='DirectRCQTests.OverfullConsumeInsertDoesNotPoisonConsumerBit:DirectRCQTests.InvalidExecutorConsumeDoesNotPoisonConsumerBit:DirectRCQTests.InvalidExecutorConsumeInsertDoesNotPoisonEntryReservation'
```

Expected: all three tests PASS.

- [ ] **Step 4.6: Commit the fix**

```bash
git add hogd/hogdb/item_lock/retrocausal_queue_lock.cc
git commit -m "$(cat <<'EOF'
fix(hogdb): roll back RCQ consumed_ and reserved_entry_count_ on early return

Previously, try_atomic_change_lock mutated consumed_ (via CAS) and
reserved_entry_count_ (via fetch_add) early, then ran fallible checks
that could return without undoing those mutations. The overfull-insert
path (#1489) rolled back the reservation but not consumed_, and every
kInvalidExecutorBehavior return leaked both.

Introduce a function-local ScopedUndo that registers an undo at each
mutation and fires on early return unless disarmed. The single success
path disarms it. The after == nullptr committed-delta undo branch is
unchanged.

Fixes #1489.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Full regression sweep of the RCQ test suite

- [ ] **Step 5.1: Run the full `DirectRCQTests` gtest class**

```bash
~/build/hogd-debug/hogd/hogexec/runtime/direct_rpc_tests/direct_rpc_test \
  --gtest_filter='DirectRCQTests.*'
```

Expected: all tests pass. Of particular interest to confirm no regression:
- `ConcurrentConsumesDisallowed`, `ConcurrentConsumesDisallowed2` — verify the existing `consumed_` release path still works.
- `StressPopSubtransactionWithConsumeDelta` — exercises sub-transaction rollback of consume deltas; depends on consume flag lifecycle.
- `StressConsumeForwardThenBackwardInSubtransactions` — exercises the narrow() path that returns kInvalidExecutorBehavior at a different layer.
- Any `Overfull*` test — exercises the cap check directly.

- [ ] **Step 5.2: If any regression, diagnose and fix**

If a previously-passing test now fails, the guard likely rolled back a mutation that another path still relied on. Most likely suspect: the `else if (this_tx_already_consumed)` branch at line 137 — we register an undo that re-stores `true`, but if subsequent code in this function then depends on `consumed_` being `false` for a success-path side effect, we need to check. Re-read the relevant test and adjust the guard registration.

- [ ] **Step 5.3: Run the broader hogdb item-lock tests (optional safety net)**

```bash
~/build/hogd-debug/hogd/hogexec/runtime/direct_rpc_tests/direct_rpc_test
```

Expected: all tests in the binary pass.

- [ ] **Step 5.4: Commit any incidental fixes (if any)**

Only if Step 5.2 required follow-up changes. Otherwise skip.

---

## Task 6: Verification and handoff

- [ ] **Step 6.1: Re-read the diff**

```bash
git log --oneline main..geoff/rcq-consumed-bit-rollback
git diff main..geoff/rcq-consumed-bit-rollback -- hogd/hogdb/item_lock/retrocausal_queue_lock.cc
```

Confirm:
- `ScopedUndo` is in an anonymous namespace (file-local).
- Copy ctor, copy assignment, move ctor, move assignment are all `= delete`.
- `disarm()` (not `dismiss()`) is the member name.
- Every successful mutation of `consumed_` / `reserved_entry_count_` in the `after != nullptr` branch has a matching `undo.push(...)` immediately after.
- The single final success return is preceded by `undo.disarm();`.
- The `after == nullptr` branch (lines 73-94) is unchanged.

- [ ] **Step 6.2: Confirm test names reflect post-fix expectation**

The three test names start with "does not poison" — they assert the fix, not the bug. They were failing; now they pass.

- [ ] **Step 6.3: Report summary to user**

Summarize: branch name, commits, test names, close-out note (`Fixes #1489.`). Ask whether to open a PR.

---

## Self-review checklist (done by plan author)

- ✅ Spec coverage: every section of the spec maps to a task. Task 4 = `ScopedUndo` + mutation/undo pairing + disarm. Tasks 1-3 = the three tests listed in the spec's Testing section. Task 5 = regression sweep (implied by "existing tests must continue to pass").
- ✅ Placeholder scan: no TBDs, TODOs, or unspecified code. Every code block is the actual content to write.
- ✅ Type consistency: `ScopedUndo` method names (`push`, `disarm`) are consistent across Task 4's steps. Test names in Tasks 1-3 match what Task 4 Step 4.5 greps for.
