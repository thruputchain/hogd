# Second hogdb payment performance pass

This pass starts at `d464e035`, after the first performance changes and a
merge of main. It measures the remaining costs rather than reusing the
previous pass's throughput baseline.

## Fresh baseline and measurement window

[Default 180-second run 34150916544](https://github.com/groundhogchain/hogd/actions/runs/34150916544)
passed at **441,505.29 tx/s**. A 60.635-second unprofiled process sample
starting at process age 30 seconds used 789.69 hogdb CPU seconds for
32,801,358 successful proposals: **24.07 CPU us/proposal**.

The long run entered a late failure regime: between process ages roughly
161 and 177 seconds, failed proposals rose from 835,196 to 8,686,000 while
successful proposals rose by only 456,132. As in the first pass, this is
consistent with the fixed expiry height and finite replay-cache capacity.
Its final cumulative rate cannot describe sustained successful-payment
throughput beyond that limit.

The workflow now offers a **120-second comparison window**, retaining its
180-second default. Both comparison versions use 120 seconds, profiling
disabled, identical transaction configuration, and the same hosts. Neither
the protocol's replay limits nor transaction expiry heights change.
Profiling requires the original 180-second window. Do not compare a
120-second optimized result against the 180-second baseline above.

The two shorter baselines are pinned to `38e064c4`, which changes only the
measurement harness:

- [34151582985](https://github.com/groundhogchain/hogd/actions/runs/34151582985)
- [34151584544](https://github.com/groundhogchain/hogd/actions/runs/34151584544)

Their results and optimized counterparts are pending. The headline metric
remains the last coordinator **overall throughput**, not the fastest block.
Transaction inputs are randomized per run despite the fixed coordinator
seed. Database CPU cost uses matched process CPU and successful-proposal
counter deltas across both executors, as described in the first report.

## Remaining costs

[Fresh diagnostic 34150918212](https://github.com/groundhogchain/hogd/actions/runs/34150918212)
passed and provides artifact `hogdb-profile-34150918212`. Its snapshot
interval was 123.286 seconds: 1,602.77 CPU seconds for 66 million successful
proposals, **24.28 CPU us/proposal**, with no failed proposals in that interval.
Perf counted 12,910,741 context switches in 120 seconds.

- Kernel call chains: **32.67%** syscall handling, including **13.55%** in
  the common vectored-I/O iterator and **12.83%** futex handling. These are
  nested categories, not additive costs. Perf scheduling itself accounts
  for part of the synchronization cost.
- Userspace self samples: **4.64%** SipHash, **1.99%** replay-cache reservation,
  **1.86%** block transaction commit, **1.55%** free, **1.36%** invocation
  commit, and **0.59%** malloc. Buffer growth/copying and RPC record parsing
  remain visible. Leaf samples are not complete subsystem totals.
- Sampled database RPC round-trip means: push **702.58 us** (64,449 samples),
  commit **703.24 us** (64,457), snapshot **1,075.80 us** (1,515). These include
  serialization, queueing, transport, server work and reply processing;
  timers stop before the executor worker-pool hop.

There were 1,114 lost samples out of approximately 207,000. Profiled
throughput is excluded from the comparison.

## Changes and validation

1. Reuse one four-byte RPC record-marker buffer for each stream's reader,
   including when reply payload ownership transfers to a caller. Fold the
   marker read into record parsing to remove an extra coroutine call.
2. Publish a pointer to the first replay-cache expiry entry after successful
   initialization. Its immutable key and snapshot data remain valid for
   the lock's lifetime; the common height can bypass the shared map lock.
   Other heights retain the existing locked lookup and capacity checks.
A third candidate reused the existing `ItemKey` and its memoized keyed hash
in the balance-lock table, trading a larger stored key for less hashing.
It was rejected: five alternating local million-payment benchmark pairs
with nonces and database hashing enabled had median phase times of 1.8246 s
without it and 1.8661 s with it (2.3% slower). Both versions passed the full
Release and Debug database suites before the candidate was removed.

The first two changes passed 262 Release database tests (three Debug-only
skips), all 265 Debug/O0 database tests, 11 RPC regression tests, four RPC
size/lifetime tests, and all five RPC stress scenarios at 128 workers in
Release and Debug/O0. New tests cover concurrent expiry-height admission,
duplicate rejection and undo, fragmented records across messages, empty
fragments, truncated input, and aggregate message-size enforcement.

Three alternating local RPC benchmark pairs at 128 workers showed a
roughly flat median rate: 1,841,126 versus 1,853,585 RPC/s (+0.68%), with
slightly higher CPU/RPC. This is not evidence of a payment throughput gain;
the matched CI comparison is still pending. Broader protocol batching remains separate work because it must
preserve transaction ordering, cancellation, and per-transaction errors.
