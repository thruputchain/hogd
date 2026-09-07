# Executor E2E performance experiment

## Throughput

Branch: `codex/executor-performance`. The optimization commit is
`9387a0e1356e01ca673e0f6f49b70e6261c8680f`, based on main at
`c2600ec00f17dcca2d7e22cc5a2073544ef05e29`. The baseline branch adds only the
opt-in profiling harness (`0d310d46b261e602eacd889c3ae5e9af1d9eb2b0`).

Both unprofiled E2E Payment runs succeeded with the same benchmark parameters:
180 seconds, 10,000 accounts, 1,000,000 transactions per block, GCC 15 Release,
and the existing eight-host schedule. TPS is the last cumulative
`Overall throughput (tx/s)` value, not a single block's rate.

| Unprofiled run | Cumulative transactions | Printed cumulative seconds | TPS |
| --- | ---: | ---: | ---: |
| [Baseline 34084880307](https://github.com/groundhogchain/hogd/actions/runs/34084880307) | 69,000,000 | 174.73 | 394,904.71 |
| [Optimized 34084882299](https://github.com/groundhogchain/hogd/actions/runs/34084882299) | 74,982,707 | 174.99 | 428,485.34 |

Observed change: **+33,580.63 TPS (+8.50%)**. This is one matched pair, not a
statistical confidence interval or a guaranteed improvement. Baseline results
varied materially across runs; profiled and unprofiled results must not be mixed.
The optimized run completed 5,982,707 more transactions in a nearly identical
cumulative interval. Printed seconds are rounded, so TPS is taken directly
from the coordinator's cumulative rate rather than recomputed from those
rounded times or averaged from the retained block records.

Immutable metrics evidence:

- [Baseline metrics](https://github.com/groundhogchain/metrics_history/blob/85d79006dc7e8ce449ea3692d6af816ec06e93f8/metrics/nightly_integration_mempool/codex/executor-performance-baseline/2026-09-07T04:16:29_0d310d46b261e602eacd889c3ae5e9af1d9eb2b0)
- [Optimized metrics](https://github.com/groundhogchain/metrics_history/blob/7036e92e06f2c0db598b6edceba1799b5aaf9e5b/metrics/nightly_integration_mempool/codex/executor-performance/2026-09-07T04:45:54_9387a0e1356e01ca673e0f6f49b70e6261c8680f)

## Profile evidence and changes

The [profiled baseline](https://github.com/groundhogchain/hogd/actions/runs/34082496846)
succeeded at 411,667.46 TPS. Each executor was sampled as its CI owner for
60 seconds, starting 25 seconds after launch, at 49 Hz using `cycles:u` and
4 KiB DWARF stack samples. Hog5 recorded 107,628 samples without reported loss;
hog6 reported 20 lost chunks and is used only as a cross-check. Inclusive
call-chain attribution was incomplete, so the numbers below are self-cycle
shares, not wall-time shares or per-optimization savings.

| Baseline cost group | Hog5 self cycles | Interpretation/action |
| --- | ---: | --- |
| x23 functions | 20.53% | RPC transport/bookkeeping remains a substantial cost; RPC synchronization was not changed. |
| Named malloc/free/operator-new functions | 7.37% | Removed owned-batch transaction copies and unnecessary NNInt coroutine frames. This group also contains other callers. |
| RV32 functions and runtime loop | 7.19% | VM execution remains necessary work; memory initialization was targeted separately. |
| Five hot libc bulk-copy-loop instructions | 5.17% | Disassembly confirms vector loads/stores. RV32 full-image copying was one avoidable source identified in code; incomplete call chains do not establish that it accounts for this entire group. |

Three implemented changes:

1. **Move owned transaction batches into execution.** Proposal and validation
   no longer deep-copy each transaction. Batch ordering, transaction IDs, the
   RPC-reader handoff, and error-result sizes are preserved.
2. **Return immediate NNInt results without coroutine allocation.** Local
   writes and cached/local reads stay immediate through both the handler and
   inner syscall wrapper. Actual database waits retain asynchronous paths,
   with unchanged errors, gas accounting, and trace output.
3. **Restore RV32 memory using seed copies and zero-fill.** Construction and
   reset copy the seeded region and zero-fill padding instead of reading the
   full zero-padded image. Every guest byte is still restored, including dirty
   recycled buffers; this is not a lazy or partial reset.

These were measured as a combined change, not isolated ablations. Admission
and commit serialization were left unchanged. The earlier x23 and hogdb
branches were preserved.

## Validation

- Release: 41 interface tests, 887 runtime tests, and 107 RV32 tests passed.
  The runtime suite skipped 40 tests and had one disabled test.
- O0/AddressSanitizer: 107 RV32 tests passed cleanly. Interface assertions
  passed (41), and runtime assertions passed (888, with 39 skipped and one
  disabled); however, both suites exited unsuccessfully with the same
  **7,510-byte / 40-allocation OpenSSL-rooted leak report observed on unchanged
  main**. The leak was not suppressed or fixed by this work.
- Added checks for immediate local/cached NNInt results, and dirty RV32 memory
  restoration across reset and pool reuse with different seed offsets.
- Builds used `scripts/build.sh`; the O0 runtime run was observed with 409
  threads. No new use-after-free or buffer-overflow report was observed.

## Optimized profile comparison

The optimized run captured 112,068 samples on hog5 and 112,480 on hog6,
with no reported lost samples or report errors. The table uses the same
self-cycle group definitions as the baseline. Values are rounded sums of
the printed per-symbol percentages, not absolute CPU time.

| Cost group | Hog5 baseline → optimized | Hog6 baseline → optimized |
| --- | ---: | ---: |
| x23 functions | 20.53% → 20.92% | 19.70% → 20.88% |
| Named malloc/cfree/operator-new functions | 7.37% → 6.92% | 6.56% → 6.77% |
| RV32 functions and runtime loop | 7.19% → 7.12% | 7.30% → 7.87% |
| Five original hot bulk-copy instructions | 5.17% → 0.00% | 5.81% → 0.02% |
| Five vector-fill-loop instructions | 0.23% → 4.62% | 0.19% → 4.37% |

The copy-to-fill shift is consistent with the RV32 restoration change.
Disassembly confirms that offsets `18d6c0`, `18d6c7`, `18d6ce`, and `18d6d5`
store a repeated vector, while `18d6dc` advances the destination. The old
copy group includes source loads. This does not establish caller attribution
for every sample or mean memory restoration became free. Allocation shares
are mixed across the two hosts; there is no basis for claiming a uniform
allocation-cost reduction from these samples alone. Approximately 3–4% of
self samples remain attributed to unknown symbols.

The 180 one-second utilization samples also show an important qualification:
estimated aggregate executor CPU work per successful proposal increased from
101.73 to 107.63 CPU microseconds on hog5, and from 100.14 to 106.99 on hog6.
These estimates divide sampled CPU seconds by successful-proposal counter
deltas over nearby, not exactly coincident, workload windows. They are not
transaction latency or committed TPS. The observed throughput gain therefore
does **not** demonstrate a corresponding CPU-efficiency gain. RPC processing
and memory initialization remain significant costs.

Local profile evidence is retained under
`/tmp/executor-e2e-analysis/profiled-{baseline,optimized}/{hog5,hog6}`;
the CI logs identify the original same-user profiling directories. Raw perf
files remain owned by the CI user; analysis uses its readable symbol reports.

## Matched profiled throughput

The [optimized profiled run](https://github.com/groundhogchain/hogd/actions/runs/34084319484)
succeeded at **441,609.41 TPS**, versus **411,667.46 TPS** for the profiled
baseline: **+29,941.95 TPS (+7.27%)**. The optimized cumulative record contains
77,212,307 transactions over a printed 174.84 seconds. Both runs used the
same opt-in profiling configuration. This second pair supports the direction
of the unprofiled result, but is not an unprofiled repeat or a statistical
confidence interval.

- [Immutable profiled baseline metrics](https://github.com/groundhogchain/metrics_history/blob/8b00a35d125af8d5f549a469e092e7fe4cfe8b20/metrics/nightly_integration_mempool/codex/executor-performance-baseline/2026-09-07T04:16:29_0d310d46b261e602eacd889c3ae5e9af1d9eb2b0)
- [Immutable profiled optimized metrics](https://github.com/groundhogchain/metrics_history/blob/1a4bc27dcb113bcf8ab40035700bd726099a3064/metrics/nightly_integration_mempool/codex/executor-performance/2026-09-07T04:45:54_9387a0e1356e01ca673e0f6f49b70e6261c8680f)

The optimized metrics commit was published at 2026-09-07 06:14:31 UTC,
after this run's workload and profiling windows. Immutable revisions are
important because rerunning the same source commit overwrites the same
metrics path.
