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

The three shorter baselines are pinned to `38e064c4`, which changes only the
measurement harness:

- [34151582985](https://github.com/groundhogchain/hogd/actions/runs/34151582985)
- [34151584544](https://github.com/groundhogchain/hogd/actions/runs/34151584544)
- [34156896258](https://github.com/groundhogchain/hogd/actions/runs/34156896258)

Optimized runs use `039b3fcb` or `f6a5cb49` (the latter changes only a test
comparison). A third pair was added after the first two optimized results
showed substantial variation. The headline metric remains the last coordinator **overall throughput**, not the fastest block.
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
the full matched CI comparison is reported below. Broader protocol batching remains separate work because it must
preserve transaction ordering, cancellation, and per-transaction errors.


## Matched 120-second CI results

Each row uses the last coordinator overall throughput. CPU samples start
at hogdb process age 32–34 seconds and span approximately 60.65 seconds.
Early and late samples have zero failed-proposal deltas in all six runs.
Exact CPU seconds, proposal counts, and sampling windows are recorded in
[hogdb-payment-round2-samples.json](hogdb-payment-round2-samples.json). The late windows start at process ages 100–105 seconds and last about
15.9 seconds. CPU/proposal is process CPU, summed over all hogdb threads,
divided by successful-proposal deltas from both executors.

| Version | CI run | Overall tx/s | hogdb CPU us/proposal |
| --- | --- | ---: | ---: |
| Baseline | [34151582985](https://github.com/groundhogchain/hogd/actions/runs/34151582985) | 414,719.64 | 23.8768 |
| Baseline | [34151584544](https://github.com/groundhogchain/hogd/actions/runs/34151584544) | 502,560.33 | 23.6136 |
| Baseline | [34156896258](https://github.com/groundhogchain/hogd/actions/runs/34156896258) | 513,646.30 | 23.7269 |
| Optimized | [34153178766](https://github.com/groundhogchain/hogd/actions/runs/34153178766) | 418,096.95 | 24.3004 |
| Optimized | [34153180535](https://github.com/groundhogchain/hogd/actions/runs/34153180535) | 517,916.31 | 23.6147 |
| Optimized | [34156897956](https://github.com/groundhogchain/hogd/actions/runs/34156897956) | 505,312.51 | 24.0453 |

Across three runs per version, mean throughput rose from **476,975.42 to
480,441.92 tx/s (+0.73%)**. Median throughput rose from 502,560.33 to
505,312.51 tx/s (+0.55%). Baseline range: 414,719.64–513,646.30; optimized
range: 418,096.95–517,916.31. This is a small observed difference relative
to the substantial run-to-run spread, not a demonstrated reliable speedup.

Mean hogdb CPU cost rose from **23.7391 to 23.9868 us/proposal (+1.04%)**.
There is no measured hogdb CPU reduction. All six unprofiled CI runs passed,
and every early and late sample had zero failed proposals. Profiling runs
and the saturated 180-second baseline are excluded from these aggregates.

## Post-change profile and next bottlenecks

[Diagnostic 34153182557](https://github.com/groundhogchain/hogd/actions/runs/34153182557)
contains the matching executable, perf data and reports, and counter
snapshots in artifact `hogdb-profile-34153182557`. Its 124.391-second snapshot
interval used 1,608.11 hogdb CPU seconds for 64,124,958 successful proposals:
**25.08 CPU us/proposal**, versus **24.28** before. Neither snapshot interval
had failed proposals. These diagnostic samples do not establish a CPU
improvement and are excluded from headline throughput.

| Profile measure | Before | After |
| --- | ---: | ---: |
| Syscall handling, cumulative | 32.67% | 32.99% |
| Futex handling, cumulative | 12.83% | 13.59% |
| Vectored I/O iterator, cumulative | 13.55% | 12.91% |
| SipHash, self | 4.64% | 4.43% |
| Replay-cache reservation, self | 1.99% | 1.76% |
| Block transaction commit, self | 1.86% | 1.90% |
| Free, self | 1.55% | 1.45% |
| Context switches in 120 s | 12,910,741 | 13,223,987 |
| Sampled push RPC mean | 702.58 us | 712.68 us |
| Sampled commit RPC mean | 703.24 us | 712.49 us |
| Sampled snapshot RPC mean | 1,075.80 us | 1,076.25 us |

The after profile lost 3,562 samples out of approximately 191,000, versus
1,114 out of 207,000 before. Sampling percentages are relative and subject
to both workload variation and profiler perturbation. Nested cumulative
categories must not be added together. The smaller replay-cache and free
shares do not imply a reduction in total CPU cost.

A further local experiment returned buffered reads synchronously through
`MaybeAsync`, retaining the asynchronous path when more data was needed.
It passed all 11 RPC regression tests, but five alternating 128-worker RPC
pairs improved median throughput by only 0.45% (1,866,734 to 1,875,212 RPC/s).
Median CPU/RPC fell about 1.0%. This small local effect did not justify the
broader public read-API change, so the experiment was removed without being
included in CI or the final implementation.

The next priorities are:

1. Reduce worker handoffs on database RPC paths, preserving cancellation,
   thread-affinity and destruction ordering. Futex and kernel scheduling
   remain much larger costs than the removed allocation/lookup work.
2. Batch independent database operations where per-transaction results and
   ordering can be preserved. The transport already reads ahead by roughly
   64 KiB; adding another RPC buffer would duplicate that mechanism. Reducing
   protocol round trips is more promising than duplicating read buffering.
3. Reuse keyed hashes without expanding the balance table's stored keys.
   SipHash remains the largest userspace leaf; the larger-key candidate's
   local regression rules out that implementation, not the underlying idea.

All nine PR-specific checks passed on `f6a5cb49`, including Release and
Debug builds, formatting, bindings, and all three tidy jobs. The final report-only commit receives the same PR checks.
