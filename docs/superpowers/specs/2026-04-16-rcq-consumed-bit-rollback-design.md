# RCQ consumed_ bit rollback symmetry

**Date**: 2026-04-16
**Issue**: [#1489 — Overfull RCQ reservation poisons the consumer bit](https://github.com/groundhogchain/hogd/issues/1489)
**Severity**: Medium (same-block RCQ DoS)

## Problem

`RetrocausalQueueLock::try_atomic_change_lock()` mutates two pieces of per-block
shared state — `consumed_` (the single-consumer flag) and `reserved_entry_count_`
(the per-block insert-budget reservation) — early in the function, then runs
several fallible checks after the mutations. Most failure paths do not undo the
mutations they already performed, so a failed first-touch transaction leaves the
lock in a poisoned state that causes unrelated, legitimate transactions on the
same key to spuriously conflict for the remainder of the block.

The documented reproduction is the overfull-insert path (issue #1489): TX2 tries
to consume + insert with an aggregate that blows past the 1000-entry per-block
cap, correctly fails with `kConflict`, but leaves `consumed_ == true`, so the
next pure consume on the same key also fails.

Sibling leaks exist on every early return between the mutations and the final
success `return`, including:

- `get_meta` failure after the `consumed_` CAS succeeds — leaks `consumed_`.
- `kInvalidExecutorBehavior` returns in the consume-state validation block
  (lines 197-234) — leak **both** `consumed_` and the already-added entry
  reservation.
- DAO `read_retrocausal_queue_block` failures in the same block — leak both.

These are less adversarially exploitable than the overfull path (the executor-
behavior returns usually imply the whole block will be rejected), but the lock
should clean up after itself regardless.

## Fix

Introduce a function-local RAII scope guard that runs registered undo actions on
destruction unless explicitly disarmed. Register an undo action immediately after
every successful mutation of `consumed_` or `reserved_entry_count_` within
`try_atomic_change_lock`. On the single success path, disarm the guard.

Name: `ScopedUndo`. It is a one-off local type — not promoted to a shared
header — because the only caller is `try_atomic_change_lock`, and a
caller-specific type avoids over-abstracting.

### ScopedUndo semantics

- Holds a `std::vector<std::function<void()>>` of undo actions.
- Has a `bool armed_` flag, initially `true`.
- On destruction, if `armed_`, invokes the actions in reverse order.
- `disarm()` sets `armed_ = false` (naming consistent with existing codebase
  convention; see `hogd/hogexec/runtime/syscall_entry_outer.h`,
  `syscall_argument_parsers.h`).
- `push(fn)` appends an undo action.
- Copy ctor and copy assignment are `= delete`. Move ctor/assignment can be
  defaulted or deleted; since we never move it, prefer deleted for maximum
  safety.
- Destructor is non-virtual (not a base class) and should not throw.

### Mutation/undo pairing inside `try_atomic_change_lock`

Within the `after != nullptr` branch:

1. After the `consumed_.compare_exchange_strong(expected, true)` CAS at line 132
   succeeds, push an undo that stores `false` back with
   `std::memory_order_relaxed`.
2. After the `else if (this_tx_already_consumed)` clear at line 139 (the update
   removed the consume), push an undo that stores `true` back.
3. After the `reserved_entry_count_.fetch_add` at line 164 passes the overflow
   check at 167, push an undo that `fetch_sub`s `additional_entries` back.
4. On the overflow failure at 167-171, **stop doing the inline rollback** —
   just `return HogDBError{kConflict}` and let the guard unwind both the
   reservation and `consumed_`. This keeps the invariant "ScopedUndo owns
   rollback" and eliminates the duplicated bookkeeping that caused the original
   bug.
5. In the `new_entries_count < previous_entries_count` branch at 174-180 (the
   mid-transaction shrink case), the `fetch_sub` reverses a reservation
   recorded by a previous call on the same transaction. Register a matching
   re-add undo, because execution continues into the consume-state validation
   block (lines 182-267) which has several fallible returns — without this
   undo, those returns would under-count the reservation.
6. Immediately before the final success `return` at the bottom of the function,
   call `guard.disarm()`.

The `after == nullptr` undo branch at lines 73-94 is untouched. It already
performs the correct rollback for committed deltas being undone via the
`ItemVersion` mechanism in `InvokeTX`, and it is never composed with
`ScopedUndo` (different call, separate invocation of `try_atomic_change_lock`).

### Why not "defer all mutations to the end"?

The entry-count reservation must happen atomically with the overflow check
against a shared counter — deferring it requires a compare-and-swap loop or
similar, which is extra complexity for no benefit. The `consumed_` CAS could
be deferred, but co-locating the two mutations with a single rollback mechanism
is simpler to audit than two different patterns.

### Concurrency notes

- All mutations use `memory_order_relaxed`, matching the existing code. The
  guard only runs on the thread that registered the undos, so no extra
  synchronization is needed.
- The guard runs in the destructor path on early return. No lock is held across
  undo.

## Testing (TDD)

Tests live in
`hogd/hogexec/runtime/direct_rpc_tests/direct_rcq_tests.cc` alongside existing
RCQ integration tests.

**Test 1 — documented overfull leak (from issue #1489):**
Replicate the PoC, but assert the *post-fix* expectation: TX3 succeeds. This
test must fail against current `main` (proving it reproduces the bug) and pass
after the fix.

**Test 2 — `kInvalidExecutorBehavior` leak of `consumed_`:**
Construct a TX that sets `delta.consume` and trips one of the executor-behavior
checks (e.g. backward `partial_consumed_number`). Submit a subsequent pure
consume on the same key in the same block and assert it succeeds. Currently
leaks `consumed_` → second consume conflicts. After fix: passes.

**Test 3 — leak of entry reservation on `kInvalidExecutorBehavior`:**
Construct a TX that reserves some entries (inserts) and then trips an executor-
behavior check further down. Submit a subsequent legitimate insert whose total
would fit under the 1000-entry cap *only if* the leaked reservation was cleaned
up. Currently leaks the reservation → subsequent insert fails on cap. After
fix: passes.

All three tests build on existing helpers (`make_new_per_tx_db_proxy`,
`make_rcq_insert_delta`, `make_rcq_consume_insert_delta`,
`make_rcq_consume_delta`) already used by `DirectRCQTests`.

Existing tests in the same file (`ConcurrentConsumesDisallowed*`,
`OverfullInsertBlock*`) must continue to pass unchanged — they exercise the
same code paths and should not regress.

## Files touched

- `hogd/hogdb/item_lock/retrocausal_queue_lock.cc` — add `ScopedUndo`, register
  undos at each mutation, disarm before success return, remove the inline
  reservation unwind at lines 169-170.
- `hogd/hogexec/runtime/direct_rpc_tests/direct_rcq_tests.cc` — three new
  `TEST_F` cases.

No header changes. No public API changes.

## Non-goals

- Refactoring the `after == nullptr` undo branch.
- Changing the memory ordering on `consumed_` or `reserved_entry_count_`.
- Factoring `ScopedUndo` into a shared utility. If a second caller needs it
  later, promote then.
- Addressing `hogd/hogdb/invoke_tx.cc:311-321` or
  `hogd/hogdb/item_lock.cc:175-222` from the issue's "Affected Code" list.
  Those are upstream call sites that forward the failure; once
  `try_atomic_change_lock` no longer leaks state, they need no change.
