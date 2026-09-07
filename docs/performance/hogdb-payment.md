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
mempool run. Host locks serialize experiments. Keep the workload, seed,
hardware, compiler and database configuration unchanged between versions.

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
RPC. Sampled mean network round-trip latencies were 754.65 and 756.32 us,
respectively (54,689 and 54,674 samples). Timers end before the executor
requeues itself onto its worker pool. These concurrent RPC latencies must
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
passed at **433,492.45 tx/s**, **2.40% above the baseline median**. Two more
optimized runs are queued; this is a preliminary single-run comparison.
The baseline range is 422,401.12–428,188.05 tx/s.

Its unprofiled process sample, starting at process age 30 seconds, measured
666.49 CPU seconds over 60.786 seconds for 28,000,000 successful proposals:
**23.80 hogdb CPU us/proposal**, **7.25% below** the baseline repeat sampled
at a comparable process age. Hogdb served more work while using roughly
the same number of CPU cores. Additional unprofiled samples and an optimized
perf diagnostic are pending. The coordinator's reported overall throughput
is the headline metric; the CPU sample's proposal rate is a separate metric.

The wakeup regression test holds early jobs busy and requires later jobs
to start in parallel, for both individual submissions and batches. All six
scheduler tests and all five RPC stress scenarios passed in Release and
Debug/O0, with 128 workers for RPC stress. Against the merged `main`, 261
database, 41 executor-interface, and 26 item tests passed. All nine
PR-specific CI checks passed on `c926181c` (Linux GCC Release/Debug, LLVM
Debug, macOS Debug, formatting, bindings, and all three tidy jobs).

## Integration and local cross-check

The isolated scheduler comparison revision is `6e2861cd`. Revision `73523c67`
also merges `main` at `90e76d17`, preserving the profiling timer around its
updated `get_db_tip(block_id, block_height)` RPC. Fixed comparison runs remain
pinned to the pre-merge revision, so unrelated main changes are not attributed
to the optimization.

A three-pair localhost RPC cross-check (128 workers per pool, 16 connections,
128 outstanding calls per connection, 256-byte echo, 10 seconds per run)
reduced median context switches from 1,793,814 to 1,056,564 (41.1%). Median
throughput changed from 1,872,303 to 1,849,628 RPC/s (-1.2%). This establishes
a reduction in scheduling churn, not a payment throughput gain. CI payment
measurements remain the deciding result.
