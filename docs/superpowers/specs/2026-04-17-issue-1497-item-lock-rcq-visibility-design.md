# Make `ItemLock::get_concurrent_entry()` finalized-only

**Issue:** [#1497](https://github.com/groundhogchain/hogd/issues/1497)
**Date:** 2026-04-17

## Problem

`ItemLock::get_concurrent_entry()` currently calls `std::visit(lock_contents_)`
without synchronization, even while `lock_state_ == kUncommitted`. That is a
data race because the uncommitted path still mutates `lock_contents_` under
`lock_` via `prepare_lock_contents()` and `reset_lock_contents()`.

The original suggested fix was to make concurrent readers lock in the
uncommitted state. After reviewing the RCQ visibility contract, that is the
wrong behavior boundary.

Existing runtime behavior already requires that uncommitted RCQ inserts are
*not* visible through the concurrent-read API:

- In `hogd/hogexec/runtime/direct_rpc_tests/direct_rcq_tests.cc`,
  `ConcurrentInsert2` expects `load_concurrent_entry(..., 0)` to return no
  entry before the first insert transaction commits.
- The visible RCQ entries are populated during
  `RetrocausalQueueLock::commit_change()`, not during
  `try_atomic_change_lock()`.

So the bug is not just "unsynchronized read." It is "this method is touching
`lock_contents_` before publication, even though the API should report no
concurrent entry at all before publication."

## Design

`ItemLock::get_concurrent_entry()` should only read `lock_contents_` after
`lock_state_` has published a finalized state.

### New rule

For this API, `kUncommitted` means "no concurrent entry is visible yet."

Implementation shape:

1. Load `lock_state_` with `std::memory_order_acquire`.
2. If the state is `kUncommitted`, return `std::nullopt` immediately.
3. Otherwise, read `lock_contents_` lock-free and keep the existing behavior:
   if the variant holds `PointerLock<RetrocausalQueueLock>`, delegate to
   `RetrocausalQueueLock::get_concurrent_entry(index)`; otherwise return
   `std::nullopt`.

This preserves the intended visibility boundary and removes the variant race
without adding a mutex on the read path.

### Why this is correct

- `lock_state_` already serves as the publication mechanism for finalized
  `lock_contents_`.
- An acquire load is sufficient because finalized publication uses release
  stores.
- Returning `nullopt` in `kUncommitted` matches the existing direct RPC test
  contract and the RCQ implementation, which only exposes committed entries.
- This avoids inventing a broader "read uncommitted under mutex" policy for an
  API that should not observe uncommitted state in the first place.

## Alternatives Considered

### 1. Lock in `kUncommitted` and read the variant under `lock_`

Rejected. It fixes the race, but it changes the mental model in the wrong
direction by implying that uncommitted RCQ state is meaningfully readable. That
would be inconsistent with the current runtime contract and with how
`RetrocausalQueueLock` materializes visible entries.

### 2. Add a generic "safe read access to `lock_contents_`" helper

Rejected for this issue. A helper that says "either hold `lock_` or observe a
finalized state" would be a reasonable abstraction if we had APIs that were
supposed to inspect uncommitted state. This API is not one of them. Adding that
helper here would make the code more permissive than the intended semantics.

### 3. Split RCQ concurrent-readable state out of `lock_contents_`

Rejected as unnecessary. The bug is fully addressed by respecting the existing
publication boundary.

## Affected Code

- `hogd/hogdb/item_lock.cc`
  Update `ItemLock::get_concurrent_entry()` to early-return in
  `kUncommitted`, and only visit `lock_contents_` after an acquire load sees a
  finalized state.
- `hogd/hogdb/item_lock.h`
  Tighten the invariant comment so it is explicit that concurrent-entry reads
  are finalized-only.
- `hogd/hogdb/item_lock_test.cc`
  Add a focused regression test for the publication boundary.

## Testing

1. Add a unit test that exercises the visibility contract directly:
   before first commit, `get_concurrent_entry()` returns `nullopt`; after
   commit of an RCQ insert, the entry becomes visible.
2. Add the TSan regression from issue #1497, or an equivalent focused race
   test, to prove the unsynchronized variant access is gone.
3. Keep the existing direct RCQ runtime test coverage (`ConcurrentInsert2`)
   as the end-to-end contract for visibility timing.

## Risks

- If any caller was accidentally relying on visibility of uncommitted RCQ
  entries, this change would preserve the documented/tested contract rather
  than that accidental behavior.
- The fix depends on `lock_state_` remaining the sole publication gate for
  finalized `lock_contents_`. Future APIs that genuinely need uncommitted reads
  should be designed explicitly rather than reusing this method.
