# RCQ Self-Consume Narrow Fix

**Date**: 2026-04-30
**Issue**: #1604
**Severity**: Critical (malicious contract triggers Unrecoverable)

## Problem

A contract can use two sequential TryInvoke child calls to the same target so the
first child inserts an RCQ entry and the second child consumes that self-inserted
entry. The executor's `get_delta()` prunes self-consumed entries from the insert
list (via `self_consumed_index_`), so the second subtransaction's delta has a
shorter `added_entries` list than the first. HogDB's
`RetrocausalQueueIt::narrow()` rejects this as `kInvalidExecutorBehavior` because
it assumes the cumulative insert list never shrinks.

## Fix

Track the high-water mark of insert count in `RetrocausalQueueIt` so that
`narrow()` accepts a shrinking insert list while preserving correct reservation
accounting in `RetrocausalQueueLock`.

### `InvokeTX::request_retrocausal_queue` (invoke_tx.cc)

Validate `delta.insert.added_entries.size() <= kRetrocausalQueueBlockEntriesMaxLen`
before constructing `RetrocausalQueueIt`. Return `kInvalidExecutorBehavior` if it
exceeds the limit. This is the RPC boundary where malformed deltas with unbounded
XDR entry counts are rejected early, before any `uint16_t` narrowing.

### `RetrocausalQueueIt` (item_change.h / item_change.cc)

Add a `uint16_t max_insert_count_` field, initialized from
`delta_.insert.added_entries.size()` in the constructor (safe after the
`request_retrocausal_queue` validation above).

In `narrow()`:
- Remove the check that rejects
  `delta_.insert.added_entries.size() < earlier.delta_.insert.added_entries.size()`.
- Replace with: `max_insert_count_ = std::max(max_insert_count_, earlier.max_insert_count_)`.

Add accessor: `uint16_t max_insert_count() const`.

Invariant: `max_insert_count_ >= delta_.insert.added_entries.size()` always holds
because the constructor initializes from the current size and `narrow()` only
raises the value.

### `RetrocausalQueueLock` (retrocausal_queue_lock.cc)

Three changes:

**1. Remove the prefix check (lines 148-163).**

The existing check verifies that the shorter insert list is a byte-level prefix of
the longer one. With self-consume pruning, `get_delta()` strips entries from the
front of `added_entries` (via `self_consumed_index_`), so the later delta is a
suffix of the cumulative list, not a prefix. This breaks the check in both the
forward path (sub2 after self-consume) and the rollback path
(`rollback_to_savepoint` restores sub1's entries which are not a prefix of sub2's
pruned list). Remove it — the real safety comes from `narrow()` (insert
validation), the block overflow check (line 231-233), and the single-consumer
constraint.

**2. Switch reservation accounting (lines 129, 213-215) to `max_insert_count()`.**

- Line 129 (unlock/full removal): unreserve `max_insert_count()` instead of
  `delta.insert.added_entries.size()`.
- Lines 213-215 (modify): use `after->max_insert_count()` for `new_entries_count`
  and `before->max_insert_count()` for `previous_entries_count`.

This pins the reservation at the peak across all narrows, so a subtransaction
rollback can always restore a prior (larger) insert list without racing against a
concurrent transaction that grabbed a freed slot. In the forward self-consume case,
`new_max == previous_max` so the reservation doesn't change. In the rollback case,
`after_max == before_max` (both inherited the same high-water mark through
`narrow()`), so the reservation also doesn't change.

**3. Keep raw `delta.insert.added_entries` at commit-time sites (lines 355, 409-416).**

Commit writes the actual entries from the final delta, not the high-water mark.
Self-consumed entries are already pruned from the delta and should not be written.
The minor inefficiency of holding `max - actual` extra reserved slots until block
end is acceptable.

### No changes to

- `RetrocausalQueueItem::get_delta()` — self-consume pruning stays as-is.
- `InvocationFrame` — frame copy semantics unchanged.
- RPC wire format — delta structure unchanged.

## Test Plan (step 1 of implementation, before any production changes)

### item_lock_test.cc — direct narrow() unit tests

- Insert list grows across narrows (existing behavior, baseline).
- Insert list shrinks (self-consume case): `narrow()` accepts, `max_insert_count()`
  stays at peak.
- Multiple narrows with mixed grow/shrink: max tracks correctly.

### direct_rcq_tests.cc — subtransaction-level RPC tests

- Push sub1 with one insert, push sub2 with zero inserts (self-consume): no
  `kInvalidExecutorBehavior`.
- Reservation stays pinned at peak after self-consume shrink.
- Rollback after self-consume restores correct reservation count.
- Partial self-consume with new appends: sub1 inserts `[A,B,C]`, sub2 emits
  `[C,D]` (consumed A,B, inserted D) — verify lock accepts and reserves 3.
- Validate `request_retrocausal_queue` rejects delta with
  `added_entries.size() > kRetrocausalQueueBlockEntriesMaxLen`.

### try_invoke_test.cc — full executor stack tests

- PoC from issue #1604: orchestrator TryInvokes insert then consume on same RCQ
  key, expect success (not Unrecoverable).
- Multiple self-consumes in sequence.
- Partial self-consume (insert 3, consume 1, insert 2 more).
- Self-consume then more inserts in later child frames.
