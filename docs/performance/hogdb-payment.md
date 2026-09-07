# Hogdb payment performance

## Experiment

Use the `E2E Payment (hog_sched)` workflow on `perf/hogdb-e2e-payment`.
The fixed baseline is `133089b24feb057d75ad5ce33695480abd64bf08`
(`perf/hogdb-e2e-baseline`): unchanged production code plus the opt-in
measurement harness. Compare the **last overall throughput** printed by the
coordinator, not the fastest block. All throughput comparison runs have
`hogdb_profile=false`; profiling runs are diagnostic only.

The existing schedule uses hog11 for hogdb and hog5/hog6 for executors,
10,000 accounts, one million transactions per input block, and a 180-second
mempool run. Host locks serialize experiments. Workload configuration,
coordinator seed, hardware, compiler and database configuration are held
constant. The payment generator independently randomizes account pairs and
nonce bases per run, so transaction inputs are not bit-identical. The
coordinator's cumulative overall rate includes the initial blocks even
though their individual reports are suppressed by `--skip 20`.

## Baseline measurements

- [Unprofiled run 34099183278](https://github.com/groundhogchain/hogd/actions/runs/34099183278):
  **422,401.12 transactions/second**, 73,996,780 committed transactions.
- [Unprofiled run 34097600690](https://github.com/groundhogchain/hogd/actions/runs/34097600690)
  at the original production revision `48d33566`: **423,351.58 transactions/second**.
- [Unprofiled run 34099186393](https://github.com/groundhogchain/hogd/actions/runs/34099186393):
  **428,188.05 transactions/second**. The three-run median is **423,351.58 tx/s**.
- During a 53.609-second interval in run 34099183278, hogdb consumed 584.33 CPU
  seconds (10.90 cores) while executors reported 23,073,859 successful
  proposals: **25.32 database CPU microseconds per successful proposal**.
  This denominator differs from committed transactions and is explicitly
  aligned with the process sampling interval. A second unprofiled sample,
  run 34099186393 at process age 32 seconds, measured 654.41 CPU seconds
  over 60.800 seconds for 25,498,509 proposals: **25.66 CPU us/proposal**.
- [Diagnostic run 34098275165](https://github.com/groundhogchain/hogd/actions/runs/34098275165)
  has the downloadable `hogdb-profile-34098275165` artifact. In its
  123.223-second snapshot interval, hogdb used 1,483.45 CPU seconds
  (12.04 cores) for 56 million successful proposals: **26.49 CPU microseconds
  per proposal**. Profiling perturbs execution; its throughput is excluded
  from the performance comparison.

The diagnostic machine is an AMD EPYC 9554, 64 cores / 128 hardware threads.
Each executor opens 32 database RPC connections through `db_connection_map`;
hogdb had 391 threads across its pools and services. Perf recorded 19,918,125
context switches over 120 seconds (166,000/second). Kernel call chains
attribute 19.90% of CPU samples to futex handling and 37.36% to system calls
in total. These are nested categories, not additive costs. Futex samples
measure CPU spent handling synchronization, not time asleep. About 5.5% of
all samples are nested in perf event scheduling during context switches;
profiling therefore inflates this category. Use the unprofiled process
snapshot above as the primary database CPU cost measurement.

Each successful payment generated approximately one push and one commit
RPC. Sampled mean client-observed database RPC round-trip latencies were
754.65 and 756.32 us,
respectively (54,689 and 54,674 samples). Timers end before the executor
requeues itself onto its worker pool. They include serialization, queuing,
network transport, server work and the reply. These concurrent latencies must
not be added up as elapsed benchmark time. Snapshot and deployment RPCs
also occur, at much lower volume.

## Bottlenecks and optimization

1. **Worker wakeup amplification.** `ThreadPool::go` previously broadcast
   to every worker on a queue when the first job arrived, including single
   RPC continuations. Most awakened workers could find no work and return
   to futex waits. Wake one worker per single job and bound batch wakeups
   by the number of newly enqueued jobs. Notify on every submission so
   already queued work cannot suppress parallel execution. Preserve
   broadcast notifications for shutdown and quiescence. This is shared
   RPC infrastructure, so it can benefit both hogdb and its clients;
   database-only CPU measurements distinguish hogdb savings from total
   system throughput gains.
2. **RPC and socket costs.** Readv/writev call chains account for about
   11.72% of samples; two hot RPCs per payment impose repeated serialization,
   network and scheduling costs. Protocol batching could reduce them, but
   needs separate work on per-transaction errors, ordering and cancellation.
3. **Hashing and transaction bookkeeping.** Userspace self samples include
   SipHash (3.93%), replay-cache lock updates (1.73%), block commit changes
   (1.69%), and allocation/free/map cleanup. An inline first-version
   experiment passed all 261 database tests, but five alternating local
   database benchmark pairs showed only a 0.7% median runtime reduction
   (1.8668 to 1.8532 seconds), with mixed pairwise results. The experiment
   was discarded: that evidence did not justify adding a new history
   container to the savepoint and rollback paths.

Userspace stack unwinding is incomplete in this perf capture, so leaf
samples do not establish complete subsystem totals. There were 1,150 lost
samples out of approximately 183,000. The sampled `/proc` I/O counters
alone do not establish whether disk I/O is a bottleneck.

## Validation and final comparison

The first unprofiled optimized run,
[34105692812](https://github.com/groundhogchain/hogd/actions/runs/34105692812),
passed at **433,492.45 tx/s**, **2.40% above the baseline median**. A second,
[34106016026](https://github.com/groundhogchain/hogd/actions/runs/34106016026),
passed at **453,453.94 tx/s**, **7.11% above the baseline median**. One more
optimized run is queued; the final three-run comparison is pending.
The baseline range is 422,401.12–428,188.05 tx/s. All runs, including the
capacity-limited second optimized run described below, remain in the comparison.

Its unprofiled process sample, starting at process age 30 seconds, measured
666.49 CPU seconds over 60.786 seconds for 28,000,000 successful proposals:
**23.80 hogdb CPU us/proposal**, **7.25% below** the baseline repeat sampled
at a comparable process age. Hogdb served more work while using roughly
the same number of CPU cores. The second optimized sample, starting at
process age 35 seconds, measured 789.97 CPU seconds over 60.622 seconds for
32,426,016 successful proposals: **24.36 CPU us/proposal**, **5.07% below**
the matched baseline sample. The last unprofiled repeat is pending.
The coordinator's reported overall throughput
is the headline metric; the CPU sample's proposal rate is a separate metric.

[Optimized diagnostic run 34106024231](https://github.com/groundhogchain/hogd/actions/runs/34106024231)
passed and provides the `hogdb-profile-34106024231` artifact. Over its
123.128-second snapshot interval, hogdb consumed 1,292.02 CPU seconds for
53,170,810 successful proposals: **24.30 CPU us/proposal**, down **8.27%**
from the baseline diagnostic. Perf counted **10,611,996 context switches**
over 120 seconds, **46.72% fewer** than the baseline capture. The snapshot
proposal count was also 5.1% lower, so the context-switch reduction is not
a normalized per-transaction measurement. Futex handling fell from 19.90%
to 13.32% of samples. Sampled push/commit RPC means fell to **720.39/718.68 us**
(51,919/51,927 samples), reductions of **4.54%/4.98%**. The less frequent
snapshot RPC mean increased from 1,182.20 to 1,245.82 us. There were 797 lost
samples out of approximately 167,000. These diagnostic results support the
wakeup mechanism; unprofiled measurements determine the throughput gain.

The wakeup regression test holds early jobs busy and requires later jobs
to start in parallel, for both individual submissions and batches. All six
scheduler tests and all five RPC stress scenarios passed in Release and
Debug/O0, with 128 workers for RPC stress. Against the merged `main`, 261
database, 41 executor-interface, and 26 item tests passed. All nine
PR-specific CI checks passed on `d316b848` (Linux GCC Release/Debug, LLVM
Debug, macOS Debug, formatting, bindings, and all three tidy jobs).

## Integration and local cross-check

The isolated scheduler comparison revision is `6e2861cd`. Revision `73523c67`
also merges `main` at `90e76d17`, preserving the profiling timer around its
updated `get_db_tip(block_id, block_height)` RPC. Fixed comparison runs remain
pinned to the pre-merge revision, so unrelated main changes are not attributed
to the optimization.

The merged-head integration run
[34108735706](https://github.com/groundhogchain/hogd/actions/runs/34108735706)
at `c926181c` passed at **418,974.74 tx/s**, below the original baseline.
Its unprofiled sample measured **23.07 CPU us/proposal** (599.78 CPU seconds
for 26 million proposals over 60.652 seconds). Lower database CPU cost does
not by itself establish higher system throughput. A matched merged-head
control, [34118747257](https://github.com/groundhogchain/hogd/actions/runs/34118747257),
is queued at `9d791472` on `perf/hogdb-merged-control`; this restores only
the original scheduler implementation and removes its new regression test.

A three-pair localhost RPC cross-check (128 workers per pool, 16 connections,
128 outstanding calls per connection, 256-byte echo, 10 seconds per run)
reduced median context switches from 1,793,814 to 1,056,564 (41.1%). Median
throughput changed from 1,872,303 to 1,849,628 RPC/s (-1.2%). This establishes
a reduction in scheduling churn, not a payment throughput gain. CI payment
measurements remain the deciding result.

## Workload capacity limitation

The second optimized repeat reached a late-run failure regime. At process
age 160 seconds, executors had recorded 79,049,241 successful and 3,489,414
failed proposals. Over the next 15.766 seconds, successful proposals rose
by only 101,176 while failures rose by 8,370,795. The coordinator still
reported its normal final cumulative rate, with 79,150,334 committed
transactions over 174.55 seconds. The merged-head integration run, by
comparison, recorded only six failures during its late seven-million-attempt
sample.

This is consistent with the workload's finite replay-cache capacity:
`groundhog-go/chuckd/sub/loadgen/loadgen.go` gives all work transactions
expiry height `1e9`, and `ReplayCacheLock` enforces 8,000 nonces per account
per expiry height. With 10,000 accounts this permits about 80 million
nonces, less initialization usage. The generic failure counter does not
identify individual conflict codes, so this attribution is an inference
from the code and observed plateau. Do not extrapolate the benchmark's
final cumulative rate to sustained throughput after that limit. A future
benchmark should rotate valid expiry heights; changing the workload during
this comparison would invalidate the original baseline.

## Reproduction

Dispatch `e2e_payment.yml` with `hogdb_profile=false` on each comparison
revision's branch. The schedule checks out the dispatch's exact commit,
even if the branch advances while waiting for hosts. Retain each completed
run's coordinator output from its GitHub log: the metrics-history file for
a revision can be overwritten by another run of that revision.

For a separate diagnostic, dispatch with `hogdb_profile=true` and download
that run's `hogdb-profile-<run-id>` artifact. It contains 120-second perf
captures, process snapshots, executor metrics, hardware metadata and, with
the updated harness, the matching executable for symbol lookup. RPC means
are the deltas of sampled nanoseconds divided by completed sample counts,
aggregated across both executors. Database CPU seconds are the delta of
`/proc/<pid>/stat` user plus system ticks divided by `CLK_TCK`; divide that
by the matched delta of both executors' successful-proposal counters to
obtain CPU cost per proposal. Separate unprofiled process/metric snapshots
use the same calculation without perf or RPC latency instrumentation.
