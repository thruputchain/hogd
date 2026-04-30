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

### `RetrocausalQueueIt` (item_change.h / item_change.cc)

Add a `uint16_t max_insert_count_` field, initialized from
`delta_.insert.added_entries.size()` in the constructor. If the initial size
overflows `uint16_t`, return `kInvalidExecutorBehavior`.

In `narrow()`:
- Remove the check that rejects `delta_.insert.added_entries.size() < earlier.delta_.insert.added_entries.size()`.
- Replace with: `max_insert_count_ = std::max(max_insert_count_, earlier.max_insert_count_)`.
- If the result overflows `uint16_t`, return `kInvalidExecutorBehavior`.

Add accessor: `uint16_t max_insert_count() const`.

Invariant: `max_insert_count_ >= delta_.insert.added_entries.size()` always holds
because the constructor initializes from the current size and `narrow()` only
raises the value.

### `RetrocausalQueueLock` (retrocausal_queue_lock.cc)

Replace uses of `delta.insert.added_entries.size()` /
`before->delta().insert.added_entries.size()` in reservation accounting (lines
213-215) with `max_insert_count()`. This prevents unreserving slots that a
subtransaction rollback could need back.

### No changes to

- `RetrocausalQueueItem::get_delta()` — self-consume pruning stays as-is.
- `InvocationFrame` — frame copy semantics unchanged.
- RPC layer — delta format unchanged.

## Test Plan (step 1 of implementation, before any production changes)

### item_lock_test.cc — direct narrow() unit tests

- Insert list grows across narrows (existing behavior, baseline).
- Insert list shrinks (self-consume case): `narrow()` accepts, `max_insert_count()` stays at peak.
- Multiple narrows with mixed grow/shrink: max tracks correctly.
- Overflow of `max_insert_count_` returns `kInvalidExecutorBehavior`.

### direct_rcq_tests.cc — subtransaction-level RPC tests

- Push sub1 with one insert, push sub2 with zero inserts (self-consume): no `kInvalidExecutorBehavior`.
- Reservation stays pinned at peak after self-consume shrink.
- Rollback after self-consume restores correct reservation count.

### try_invoke_test.cc — full executor stack tests

- PoC from issue #1604: orchestrator TryInvokes insert then consume on same RCQ key, expect success (not Unrecoverable).
- Multiple self-consumes in sequence.
- Partial self-consume (insert 3, consume 1, insert 2 more).
- Self-consume then more inserts in later child frames.
