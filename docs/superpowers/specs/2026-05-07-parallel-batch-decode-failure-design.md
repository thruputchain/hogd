# Parallel Batch Decode Failure Handling Design

**Date:** 2026-05-07
**Issue:** [#1831](https://github.com/groundhogchain/hogd/issues/1831)
**Severity:** High (public malformed input can reach C++ undefined behavior)
**Subsystem:** `hogd/base`, `hogd/mempool`

## Problem

`parallel_batch_to_t()` assumes each `ParallelBatch` segment contains exactly
the number of XDR objects claimed by `segment_tokens[i]`. A malformed batch can
preserve the top-level metadata invariants while overclaiming the number of
objects inside a segment. If a segment contains one serialized object but
claims two tokens, the second iteration forms a view from one past the end of
`parallel.xdr` before `deserialize_from()` can reject the input.

The same helper is used for public transaction submission and for validating
consensus nominations. Call sites currently wrap parse results in `CHECK`, so a
recoverable decode failure can still terminate the process.

## Existing Metadata Contract

The C++ decoder is allowed to keep the current top-level metadata checks as
invariants:

- `segment_tokens.size() == segment_bytes.size()`
- the sum of `segment_bytes` equals `xdr.size()`

For consensus validation, the Go coordinator is responsible for checking those
top-level metadata properties before calling into the scheduler. The relevant
paths are:

- `groundhog-go/chuckd/util/xdrutil/batchutil.go:ValidateBatch`
- `groundhog-go/chuckd/util/protoutil/consensus.go:IsSaneConsensusValue`
- `groundhog-go/chuckd/sub/baton/baton/vconsensus.go:tryValidateConsensusFast`

Those checks do not prove that the per-segment bytes decode into the claimed
number of objects. Object decodability remains the C++ scheduler's
responsibility.

## Design

### Decoder Contract Comment

Document `parallel_batch_to_t()` in `hogd/base/xdr_util.h` with its required
precondition:

- Callers must only pass `ParallelBatch` values whose segment metadata has
  already been validated.
- Violating that metadata precondition remains a programmer error and is
  enforced by `CHECK_EQ`.
- Malformed per-object XDR within otherwise sane segment metadata is untrusted
  input and must be reported by returning `false`, not by forming invalid
  pointers or crashing.

This keeps the C++ helper's split responsibility explicit instead of implying
it is a full `ParallelBatch` validator.

### Segment Decode Bounds

Within each segment, before constructing the per-object `BufView`, check:

```cpp
if (inner_xdr_offset >= num_segment_bytes) {
  info("failed to deserialize: segment {} claims more tokens than bytes", i);
  co_return false;
}
```

The check must run before taking the address of a byte in `parallel.xdr`.
Positive token claims against an empty segment, or any overclaim after the last
object consumes a segment, will therefore return `false` without indexing
one-past-end.

After each successful deserialize, advance `inner_xdr_offset` and defensively
reject `inner_xdr_offset > num_segment_bytes`. After all claimed tokens in the
segment have been decoded, require `inner_xdr_offset == num_segment_bytes`.
Leftover bytes in a segment are malformed because `t_to_parallel_batch()`
serializes each object back-to-back and records the exact segment byte count.

### Public Submit Paths

In `MempoolCoordinatorRpc::schedulercoord_submit_unvetted()` and
`schedulercoord_submit_vetted()`, replace parse-result `CHECK`s with
recoverable handling:

- If `parallel_batch_to_t()` returns `false`, log the malformed batch and
  return from the void RPC handler.
- Do not verify auth, add transactions, or notify downstream components after
  parse failure.

This prevents malformed gateway-originated transaction batches from terminating
the scheduler process.

### Consensus Validation Path

In `MempoolCoordinatorRpc::schedulercoord_validate()`, the nomination parse
stage should treat decode failure as invalid consensus data:

- Start both parse coroutines as today.
- Await both handles so no coroutine is abandoned.
- If either returns `false`, decrement `num_validations_in_progress_`, set
  validation telemetry to `SchedulerValidationInvalid`, log the parse failure,
  and return `SchedulerValidationResult(SchedulerValidationInvalid)`.

The top-level metadata `CHECK`s inside `parallel_batch_to_t()` remain in force
because the Go coordinator must already have run the sanity checks described
above. Decode failure is different: it is adversarial input embedded in a
metadata-sane nomination and must produce the invalid-block signal.

## Testing

Add focused tests around `parallel_batch_to_t()` in
`hogd/base/xdr_util_test.cc`:

- A valid one-transaction `ParallelSignedTransactionBatch` round-trips.
- Overclaiming `segment_tokens[0]` on an otherwise metadata-sane batch returns
  `false` instead of triggering a death test.
- Appending leftover bytes to a segment while updating `segment_bytes` and
  `xdr` consistently returns `false`.

Add a mempool validation test in `hogd/mempool/test/mempool_test.cc` using the
same setup pattern as `TEST(Mempool, Validate)`:

- Mutate one nomination batch so metadata remains sane but object decoding
  fails.
- Assert `schedulercoord_validate()` returns `SchedulerValidationInvalid`.

## Non-Goals

- Do not move Go's metadata sanity checks into C++ as recoverable parser
  behavior.
- Do not change the `ParallelBatch` XDR schema.
- Do not introduce structured parse error enums unless a later caller needs
  richer diagnostics than `bool`.
