# RCQ stacked-subtransaction rollback coverage

**Date**: 2026-04-17
**Follow-up to**: [#1489 fix — PR #1491](https://github.com/groundhogchain/hogd/pull/1491)
**Scope**: Test coverage + comment-only documentation. No production logic changes.

## Problem

PR #1491 introduced a `ScopedUndo` RAII guard in
`RetrocausalQueueLock::try_atomic_change_lock` and shipped three regression
tests exercising the *first-touch* shapes:

1. `OverfullConsumeInsertDoesNotPoisonConsumerBit` (CAS + fetch_add undo on overflow)
2. `InvalidExecutorConsumeDoesNotPoisonConsumerBit` (CAS undo on invalid consume)
3. `InvalidExecutorConsumeInsertDoesNotPoisonEntryReservation` (CAS + fetch_add undo on invalid consume)

All three use `before = nullptr` (first-touch). The guard also runs on **stacked**
subtransaction calls (`before != nullptr`), which take different branches
through the mutation code — most notably the `this_tx_already_consumed = true`
case where no CAS is performed but the reservation `fetch_add` still runs.
That path has no regression test today.

Additionally, two of the four rollback sites in the guard are **not reachable**
via any end-to-end test:

- **else-if clear undo** (`before.consume && !after.consume` + fail downstream):
  blocked by `RetrocausalQueueIt::narrow()` at `item_change.cc:114`, which rejects
  dropping consume in forward sub progression. The only legitimate caller
  producing this shape is `pop_subtransactions`, whose lock call always has
  `new_entries ≤ previous_entries` (shrink) and therefore never hits the
  overflow check — no failure path exists downstream of the else-if clear.

- **shrink `fetch_sub` undo** (`new_entries < previous_entries` + fail
  downstream): narrow rejects shrinking inserts forward, and pop's call can't
  fail for the same reason.

These undos are defensive: they preserve the "`ScopedUndo` owns rollback"
invariant so that any future edit adding a fallible step after the mutation
is automatically covered. That invariant is currently enforceable only at
code-review time.

## Goals

1. Extend regression coverage to stacked-sub shapes that ARE reachable.
2. Document the two defensive-only undos inline so a future editor knows the
   invariant they preserve.

## Non-goals

- No new production code. No changes to `ScopedUndo`, `try_atomic_change_lock`,
  or lock-state observability.
- No unit test file, no friend decl, no header changes.
- No attempt to exercise the else-if or shrink undo paths via a synthetic
  backdoor.

## Design

### Tests to add

Four `TEST_F`s appended to
`hogd/hogexec/runtime/direct_rpc_tests/direct_rcq_tests.cc`. Each uses the
existing `DirectRCQTests` fixture and helpers (`make_rcq_insert_delta`,
`make_rcq_consume_insert_delta`, `make_rcq_consume_delta`,
`make_new_per_tx_db_proxy`, `setup_grant_storage_token`).

**1. `StackedInsertThenInvalidConsumeInsertDoesNotPoison`** — CAS + fetch_add
undo via invalid-consume failure in a stacked sub.

- sub1: insert-only, 100 entries. Succeeds. Lock state: `consumed_=false`,
  `reserved_entry_count_=100`.
- sub2: `consume_insert` with `max_fully_consumed_block_number = 1` on a
  fresh RCQ (where `max_live_block_num = 0`), cumulative inserts = 300.
  narrow passes — inserts grow monotonically, and adding a consume where
  earlier had none is not blocked.
- Lock entry for sub2: `before.consume = null`, `after.consume != null`,
  `this_tx_already_consumed = false` → enters the `if (delta.consume)` branch:
  CAS `consumed_` to true + undo pushed, `fetch_add(200)` + undo pushed,
  consume validation trips `kInvalidExecutorBehavior` on line ~244. Guard
  unwinds both.
- Post-sub2: `consumed_=false`, `reserved=100` (indirect observation below).
- sub2's `try_push_current_subtransaction` returns `!r.has_value()` (matches
  existing `ASSERT_FALSE(r.has_value())` pattern for invalid-executor returns).
- Commit sub1. Run a separate tx that does a pure consume on the same key in
  the same block → must succeed (proves `consumed_` was not leaked).

**2. `StackedInsertThenOverfullConsumeInsertDoesNotPoison`** — CAS +
fetch_add undo via **overflow** failure (different failure leg than test 1,
same undos).

- Filler tx (separate): insert 800 entries on the same key. Reservation = 800.
- tx1 sub1: 100 inserts. Reservation = 900, `consumed_=false`.
- tx1 sub2: `consume_insert` with VALID consume, cumulative inserts = 300.
  `additional = 200`. `fetch_add(200)` → reservation would be 1100, overflow
  at line ~220 returns `kConflict`.
  - CAS already set `consumed_=true` + undo pushed. fetch_add undo pushed.
    Both fire on the overflow return.
- sub2's push returns `r.has_value() && !r.value()` (standard kConflict shape).
- Commit sub1. Verify via a separate tx's pure consume that `consumed_` is
  not leaked.

**3. `StackedConsumeInsertThenInvalidConsumeInsertDoesNotPoison`** —
fetch_add undo when `this_tx_already_consumed = true` (CAS skipped, only
reservation undo in play).

- sub1: `consume_insert` with valid consume, 100 inserts. Succeeds.
  `consumed_=true, reserved=100`.
- sub2: `consume_insert` with **invalid** `max_fully_consumed=1`, cumulative
  300 inserts. narrow passes (consume is monotonic since both ends have
  consume; inserts grow).
- Lock entry: `this_tx_already_consumed=true`, `delta.consume != null` → no
  CAS. fetch_add(200) + undo pushed. Consume validation fails. fetch_add
  undo runs; `consumed_` remains true (unchanged).
- Post-sub2: `consumed_=true, reserved=100`.
- sub2's push returns `!r.has_value()`.
- Commit sub1. Post-commit reservation is 100 (post-fix) or 300 (leaked —
  200 from sub2 not rolled back). A separate tx inserts **900** entries on
  the same key in the same block: `100+900=1000 ≤ cap` passes (post-fix);
  `300+900=1200 > cap` fails (leaked). Asserting the 900-insert tx succeeds
  pins the reservation down to ≤ 100.

**4. `PopConsumeAddingSubReleasesConsumerBit`** — positive coverage of the
else-if CLEAR (reachable via pop; its undo is NOT reachable but its
happy-path IS).

- sub1: insert-only, 1 entry. Succeeds.
- sub2: `consume_insert` — adds consume + 1 more insert (cumulative 2).
  Succeeds. `consumed_=true`.
- `pop_subtransactions(1)` pops sub2. Lock call: `before = sub1+sub2 (has
  consume)`, `after = sub1 (no consume)` → `this_tx_already_consumed = true`,
  `delta.consume = null` → else-if branch CLEARs `consumed_` and pushes the
  restore-to-true undo. Reservation shrinks (fetch_sub), undo pushed. No
  consume-validation block runs (after.consume null). Final return →
  `undo.disarm()`. Both undos discarded. Lock state: `consumed_=false`,
  reservation = sub1's count.
- Separate tx issues a pure consume on the same key → must succeed.
- Commit sub1.

### Inline documentation

Add a short comment block at each of the two unreachable-undo sites in
`hogd/hogdb/item_lock/retrocausal_queue_lock.cc`:

**At the else-if branch** (currently around line 186):

> NOTE: The undo below is defensive. Today this branch is only reachable via
> `pop_subtransactions`, whose lock call has `new_entries ≤ previous_entries`
> (shrink) and thus never hits the overflow check — no failure path exists
> downstream. `RetrocausalQueueIt::narrow()` blocks forward drops. The undo
> preserves the `ScopedUndo` "owns rollback" invariant so future edits that
> add a fallible step after this point stay correct.

**At the shrink fetch_sub branch** (currently around line 234):

> NOTE: Same defensive-undo rationale as the else-if branch above: the only
> shrink caller is `pop_subtransactions`, which cannot fail downstream today.
> Kept for invariant preservation across future edits.

### Observability strategy

Tests assert lock state **indirectly** via end-to-end behavior:

- `consumed_`: test with a separate tx's pure consume on the same key in the
  same block. Success = `consumed_` was false at that moment; conflict =
  `consumed_` was true.
- `reserved_entry_count_`: test with a separate tx's insert of a sized
  amount. A boundary size (e.g., 900 after the subject sub's 100) succeeds
  iff total reservation ≤ the cap — tells us what the current reservation
  is, modulo other committed txs.

No internal atomics are read directly. No friend decl is needed.

## Files touched

- `hogd/hogexec/runtime/direct_rpc_tests/direct_rcq_tests.cc` — append four
  `TEST_F`s at EOF (inside `namespace hogd`, matching the file's existing
  convention).
- `hogd/hogdb/item_lock/retrocausal_queue_lock.cc` — add the two NOTE
  comments at the defensive-undo sites.

## Risks

- **Test 3's indirect reservation check depends on no other concurrent
  contention on the key in the same block.** Tests run serially in a single
  process, so this holds.
- **All four tests rely on the fix in `1ba3edfc6` + `15556ea9e`.** Without
  the fix, tests 1–3 flip from passing to failing (they're regression tests
  for the fix). Test 4 is a positive-path test that should pass on both
  buggy and fixed code.

## Testing

Build: `cmake --build vscode --target direct_rpc_test -j8`
Run target: `vscode/hogd/hogexec/runtime/direct_rpc_tests/direct_rpc_test --gtest_filter='DirectRCQTests.Stacked*:DirectRCQTests.PopConsume*'`
Regression sweep: `vscode/hogd/hogexec/runtime/direct_rpc_tests/direct_rpc_test --gtest_filter='DirectRCQTests.*'` — all must pass, no regressions.
