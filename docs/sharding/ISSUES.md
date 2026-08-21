# Issues

A running log of known issues found while building and benchmarking hogdb
sharding. Newest first. Each entry: status, where, what, why, repro, artifacts.

**Numbering is stable and has gaps.** Issues 1, 4 and 5 were fixed and verified
on-cluster, and were removed from this log on 2026-08-21 rather than renumbered,
so that the cross-references in the entries below keep pointing at the right
thing. Their full write-ups are in this file's git history (before the
"sharding: prune the fixed issues" commit) if the forensic detail is ever wanted
again:

| removed | what it was | outcome |
|---|---|---|
| 1 | mempool SIGSEGV in `Scheduler::process_transfers`; 2-shard runs stalled at block 0 | FIXED + verified on-cluster 2026-07-21 (genesis-readiness barrier + genesis dedup) |
| 4 | auth server not sharded; 2-shard runs auth-failed 100% of the load | FIXED + verified on-cluster 2026-08-04 (`3001b393`); a full 2-shard run committed 79.5M txs with 0 auth failures |
| 5 | genesis reported complete before its hog accounting landed | FIXED 2026-08-04 |

Issues 6 and 2 are kept in compressed form: their crashes are fixed, but each
left an open defect behind, and later entries reference them.

---

## 0. standing hazard: a failed `CHECK` in a Release build is undefined behaviour

- **Status:** OPEN, repo-wide, and the highest-value fix in this whole log. Not
  sharding-related. Recorded separately because it is the *mechanism* by which
  three different defects in this log reached the process as unreadable crashes
  rather than one-line diagnostics.
- **Where:** [`x23::panic`](x23/x23/debug.h#L191) — under `-DNDEBUG`, `CHECK`
  failure goes to `std::unreachable()`.

Under `-O3` that is UB, so the observable "crash" is whatever the optimiser left
behind. It has already presented as a `free(): invalid pointer` on a code
address, a `SIGSEGV` in `__cxa_begin_catch` with a nonsense
`_Unwind_Exception*`, and a silent death with no log line at all — three
signatures, one cause, roughly four days of forensic work between them.

**Every `CHECK` in a Release build is a latent UB SIGSEGV.** The three
occurrences so far are issue 6 (a db `DbSchedResultError` from ENOSPC), issue 7
(the per-shard conservation invariant) and removed-issue 1. Two of the three
were db results reaching a `CHECK` in the mempool's `commit_block`.

**Two fixes, and they are independent:**

1. **`CHECK` must abort with a diagnostic under `NDEBUG`.** This is a deliberate
   repo-wide behaviour change and should be decided as one, not smuggled in with
   a bug fix — which is why this entry flags it rather than doing it.
2. **A db error must not be a `CHECK` in the mempool.** `commit_block` should log
   the `DbSchedResult` and fail the block. Two unrelated defects have now reached
   the process through that one line, and issue 10 adds a third path (an
   unguarded `co_await` in a detached coroutine) to the same call site.

---

## 12. the storage filler worked on wasm only, and wasm is gone

- **Status:** OPEN, found 2026-08-21 while rebasing onto `main`. Blocks the
  capacity experiments that need to grow state faster than erc20 transfers can.
  Its four tests are `DISABLED_` on the sharding branch with this diagnosis
  inline (`hogd/loadgen/erc20_experiment_tests.cc`).
- **Where:** `StorageFiller` in `built_contracts/integration_test_contracts`
  (branch `vp/storage-filler-on-main`), driven from
  [`erc20_experiment.cc`](hogd/loadgen/erc20_experiment.cc) via
  `filler_gas_limit()`.

**What.** Every filler number ever recorded was measured against the **wasm**
artifact — the 8,243 gas for one 1 KiB write and 15,335 for four that
`filler_gas_limit()`'s comment quotes are literally what the fixture printed.
Upstream deleted wasm (#3465), so the filler now loads its rv32 artifact, and on
rv32 the invocation **does not succeed at all**: `measure()` returns `nullopt`
before it ever reports gas.

**It is not the gas budget.** Raising `filler_gas_limit()` from 107,500 to
100,000,000 fails identically, so the transaction is not running out of gas — the
rv32 invocation itself is failing. Confirmed by running the pre-rebase Aug-18
binary side by side: it prints `filler 1 x 1KiB: gas=8243` and passes, because
that tree still built wasm.

**Why it matters.** This is the only reason the sharding PR pins the
`integration_test_contracts` submodule at a branch commit. Porting the contract
to rv32 removes an unmerged dependency from that PR; abandoning the filler
removes it too. Either resolution is progress — leaving it as-is is the only bad
option.

**Repro:** `./build/hogd/loadgen/loadgen_tests
--gtest_filter='StorageFillerTest.*' --gtest_also_run_disabled_tests`

---

## 11. genesis is one RPC message, so any bench or run above ~135k accounts dies on the 16 MB cap

- **Status:** OPEN, low severity, easy to work around by using fewer accounts. Hit while sweeping `multiexec_runner` at `-a 1000000` (2026-08-20).
- **Where:** `dbsched_load_genesis_configuration` — the whole `GenesisConfiguration` is serialized into a single RPC message.

The call dies rather than degrading:

```
terminate called after throwing an instance of 'x23::XdrBadInput'
  what(): DbSched1::dbsched_load_genesis_configuration:
          message size 124000612 exceeds maximum 16777216
```

**Why it matters.** At ~124 bytes per account the ceiling is about 135,000
accounts, whatever the machine has for memory. That is a hard limit on how wide a
keyspace any experiment can start from, and a wide keyspace is exactly what a
sharding measurement wants — with a narrow one the state stays lumpy and the
extra shards only add fan-out cost. It also fails as a `terminate()` rather than
an error return, so a harness sees a core rather than a message.

**Repro.** `./build/hogd/hogexec/bench/multiexec_runner -s hog1 -s hog2 -s hog6 -a 1000000 -d 1`

**Fix options.** Chunk genesis across messages (the natural fix, since each shard
only keeps the items it owns anyway), or raise the max message size for this one
call. Either way it should return an error instead of throwing through the
coroutine.

---

## 10. at a 50M-tx backlog hogdb dies silently and takes the mempool down with it (1 shard, 41 GB mempool RSS)

- **Status:** OPEN. Both deaths are confirmed from logs and cores; **hogdb's root cause is pending analysis of its core** (`/tmp/vinita/hogdb.3561296.core` on hog1, 15.7 GB, tmpfs — will not survive a reboot). Found on the first genuine arm of the 50M-limit crossover ladder (`1shard-xover50-02`, 2026-08-17 13:09–13:17).
- **Where:** primary failure unknown, inside hogdb on hog1. Secondary failure at [`hogd/mempool/rpc_from_db.h:73-133`](hogd/mempool/rpc_from_db.h#L73-L133), `MempoolDbRpc::handle_transfer`.

**What happened, in order.** The mempool filled to the new 50M limit (`submit_unvetted rejected txs as the mempool has too many pending txs`, once a second, from work batch ~150) at **41.8 GB RSS** — about 0.86 KB per pending transaction. Then:

| time | event |
|---|---|
| 20:15:48 | hogdb logs a normal `committed block`, `mem stats: 9.314GiB (79% memory items, 10% disk buffering, 10% free)`, `bytes enqueued == bytes written` (7.408 GiB). Healthy. |
| 20:15:48 | **hogdb's per-second logging stops dead.** No error, no `CHECK`, no `panic`, no `terminate` — its log simply ends. It left a 15.7 GB core. |
| 20:16:07 | mempool: `commit_block for block ... failed: dbsched_commit_snapshot: RPC connection closed before reply`, then `RpcStreamImpl: read(fd 33, n 252000212) returned 46538716 bytes` — a **252 MB** db message truncated mid-flight, consistent with the peer dying mid-write. |
| 20:16:07 | mempool `terminate()`s: `Unhandled exception in coroutine ... What: RPC call on failed file descriptor`, stack in `MempoolDbRpc::handle_transfer`. 40.9 GB core. |

hog1 had 426 GB free of 755 GB and hog10 ~320 GB free, `dmesg` shows no OOM kill on either, and an OOM `SIGKILL` would not have produced a core. So neither death is memory exhaustion by the machine.

**Why the mempool died is already clear, and it is a defect independent of hogdb's.** `handle_transfer` is an `x23::Detached` coroutine that `co_await`s `process_transfers` and `write_all_average_hog_ages_to_db()`, both of which do db RPCs. When the db connection is gone those throw `XdrException`, and an exception escaping a *detached* coroutine terminates the process. **This is the same defect as #2** — an unguarded `co_await` on a dropped connection killing the mempool instead of reporting a db failure — at a third call site. #2's fix was applied to `coordscheduler_notify_vetted`; the db transfer path never got it.

**What this says about the 50M limit.** It is the first configuration to actually reach a 50M backlog, and reaching it is what exposed both defects: 41.8 GB of held transactions and a 252 MB db message are both roughly 8x what the 5M limit could produce. The limit change did not *create* either bug — #2's shape predates it — but a larger backlog makes both far easier to hit, so **raising this constant should not ship before the detached-coroutine guard does.**

**Repro:** `shard-bench/bin/xover50.sh`, 1-shard arm. Note the 1-shard arm of this configuration has never once completed, at either limit: at 5M it produced two `NO_START`s and two `STALL`s (`1shard-xover-01` through `-04`), and at 50M it crashes here.

**Artifacts:** `/home/hog12/vinita/shard-bench/results/1shard-xover50-02/` — progress trace with the RSS climb, both logs, failure scan. Cores: `hogdb.3561296.core` on hog1 and `mempool_exec.4049236.core` on hog10, both 13:16.

---

## 9. every benchmark abandons its mempool backlog: the load driver stops nominating when its batch list runs out

- **Status:** OPEN, and it changes how every capacity number in this campaign should be read. **Not sharding-related, and not caused by the mempool tx limit** — found while A/B-testing that limit (below), and present identically on both sides of it.
- **Where:** [`groundhog-go/chuckd/system/coordinator_loadgen/system.go:258-277`](groundhog-go/chuckd/system/coordinator_loadgen/system.go#L258-L277) — the submit loop; the misleading log line is at `:275`.

**What:** a run ends with millions of *admitted* transactions sitting in the mempool that are never committed and never will be. Both arms of the limit A/B finished with the scheduler full:

| | `vacct-5m-verify` (5M limit) | `vacct-50m-01` (50M limit) |
|---|---|---|
| offered | 20,000,000 | 20,000,000 |
| `submitted_unvetted_tx_count` | 12,020,002 | 19,020,002 |
| `rejected_capacity_rpc_count` | 7 | 0 |
| `discarded_capacity_tx_count` | 1,000,000 | 0 |
| `commit_removed_tx_count` | 6,020,002 | 9,020,002 |
| **left pending at the end** | **5,000,000** | **10,000,000** |

**Why.** The driver loop logs `waiting for work batch %d to commit` and then does not wait for any commit — it immediately loops back to `WaitWorkTxs` for the next loadgen batch. With `--num-blocks 20` exhausted, that blocks forever on a batch the loadgen will never produce. Both runs' last line is `waiting for work tx batch 20 to arrive from loadgen`, ~3 minutes before the harness gave up. Block production stops there, so the residual backlog is not drained, it is *abandoned*. The log line is also simply false about what the code does, which is the same defect shape as #8's `panic: e.g. memory allocation failure`.

**Consequences worth acting on:**

1. **Every `N of M txs committed` figure in this campaign is a lower bound on what the system could commit**, short by however much was still pending — 5M and 10M in the two runs above. Comparisons between arms remain valid (all arms are cut off the same way); absolute capacity claims are not.
2. **A plateau in committed transactions does not mean "drained".** The harness treats four stable polls as a drain, which cannot distinguish a finished workload from an abandoned backlog. It now records `scheduler_pending_tx_count`, so the two are at least separable after the fact.
3. **The fix is cheap and it is in the driver, not in hogd:** after the last batch, keep nominating until `scheduler_pending_tx_count` reaches 0 (or until it stops falling) before reporting the run finished. Until that exists, no run measures drain rate at the tail.

### The mempool tx limit A/B that found it

`kDefaultMempoolTxSizeLimit` ([`hogd/mempool/rpc_from_coord.h:66`](hogd/mempool/rpc_from_coord.h#L66)) raised 5,000,000 -> 50,000,000, uncommitted, nothing else changed, same configuration replayed. The limit gates `scheduler_->size()` in [`schedulercoord_submit_unvetted`](hogd/mempool/rpc_from_coord.cc#L216-L281) — it bounds the *pending backlog*, not a run's lifetime total, so it is not the hard ceiling on committed transactions that it looks like. What it does bound is how much of a burst survives admission, and that was biting hard: at 5M the mempool turned away 8M of the 20M offered (7 whole RPCs rejected outright, 1M truncated) and pinned at exactly 5,000,000 held. At 50M it rejected and discarded **nothing**, admitted 19M, and committed **50% more** (6.02M -> 9.02M).

**Cost side:** ~1.2 KB of mempool RSS per pending transaction — 12.1 GB peak at 10M held, so a full 50M backlog would be ~60 GB on the mempool host (hog10 has ~320 GB free, so it fits, but it is no longer negligible). The benchmark harness now samples mempool RSS into `progress.csv` for exactly this reason.

**Still open:** the `TODO(jhumphri)` asking for the limit to be a command-line arg. Raising the constant is enough to measure with; it is not the shipping answer.

**Repro:** `shard-bench/bin/txlimit.sh` (replays `vacct-5m-verify` against the raised constant), then `shard-bench/bin/txlimit-report.sh` for the table above.

**Artifacts:** `/home/hog12/vinita/shard-bench/results/{vacct-5m-verify,vacct-50m-01}/` — both runs' mempool metrics, chuckd tails and progress traces. Note `vacct-50m-01` saw 1741% foreign CPU on hog8 at t=72, so its *timing* is contended; the admission counters above are not affected by that.

---

## 8. the event server's work dir is never deleted, fills the log tmpfs, and takes the whole run down with it

- **Status:** launch-multi.sh leak FIXED 2026-08-11 (uncommitted); the two robustness defects it exposed are OPEN. **Not sharding-related** — it killed 1-shard and 2-shard runs alike. Found while re-running the sharded-vs-unsharded benchmark, and it is the same shape as #6: an unbounded write into a tmpfs, surfacing as a failure nowhere near the cause.
- **Where:** [`launch-multi.sh`](scripts/launch-multi.sh#L256-L262) deleted the indexer db, chuckd-storage and the hogdb data dir between runs, but never `$logdir/event`, which is `event_exec --work-dir`.

**What:** five consecutive runs stalled, each one **earlier than the last**, with block production frozen and the mempool still alive:

| run | started | stalled at work batch | `event_exec` core on hog9 |
|---|---|---|---|
| s1-r1 | 20:39 | 776 | 21:07 |
| s2-r2 | 21:25 | 505 | 21:42 |
| s1-r2 | 21:59 | 334 | 22:10 |
| s2-r3 | 22:28 | 222 | 22:34 |
| s1-r3 | 22:52 | 143 | 22:56 |

The monotonic decrease is the diagnosis: something cumulative, not a race. `/tmp/vinita/event` had reached **146 GB of hog9's 189 GB tmpfs (84% full)**, so each run had less headroom than the one before and `event_exec` died sooner. There is one core per stalled run.

**The failure chain, and why it reads as something else entirely.** `event_exec` dies, and then:

```
mempool  E rpc_from_coord.cc:1454 scheduler proposal cleanup
           'abandon proposal event block' failed: RPC call on failed file descriptor
mempool  E rpc_from_coord.cc:1454 scheduler proposal cleanup
           'finish proposal executor' failed: bad_function_call
chuckd   panic: e.g. memory allocation failure
           must.NoError -> baton.propose -> propose.go:69
```

**`panic: e.g. memory allocation failure` is not an OOM** — hog4 had 331 GB free. It is chuckd printing the *comment text* of `SYSTEM_ERR = 5` from [x23/x23/rpc_msg.x:38](x23/x23/rpc_msg.x#L38) as if it were an error message. Two days of this campaign were nearly spent chasing a memory leak that does not exist.

### Three defects, one symptom

1. **The leak (fixed).** `launch-multi.sh` now `rm -rf`s `$logdir/event` under `--delete-db`, next to the indexer. ~20 GB per run was accumulating.
2. **`must.NoError` is fatal on any RPC error (OPEN, groundhog-go).** A single failed `propose_block` kills chuckd and therefore the run. This is the same defect as #3, at a different call site — worth fixing once, generally, rather than per-site.
3. **The error message is a lie (OPEN, cheap).** Whatever renders an x23 RPC error should print the enum *name* (`SYSTEM_ERR`), not the illustrative comment beside it. One-line change, large diagnostic value — the same lesson as #6's missing errno.

**Also worth deciding:** `event_exec` aborts on a failed write rather than degrading. Every service in this system writes into `$logdir` without bound, and only hogdb's data dir has been moved off tmpfs (#6). A disk-space budget for a long run is still nobody's job.

**Harness change made alongside:** the benchmark harness now refuses to start a run when any host's `$logdir` is over 70% full, and wipes every regenerable artifact on all nine hosts before and after each run, printing free space both times. Both classes of fill would now fail at t=0 with the top consumers named.

**Artifacts:** `/home/hog12/vinita/shard-bench/stalled-eventdir-runs/` on NFS — all five runs' harness logs, mempool logs, chuckd panics and progress traces. Cores `event_exec.24{07780,08984,10064,11032,11950}.core` on hog9 `/tmp/vinita` (tmpfs, will not survive a reboot).

---

## 7. hogdb's hog-conservation invariant is per-shard, so any assertion-enabled multi-shard run dies at genesis

- **Status:** OPEN (found 2026-08-10 @ `97c4f6cc`, while setting up the sharded-vs-unsharded benchmark). **Not sharding-correctness** — the data is fine and the shards agree; the *check* is wrong. Only bites builds with assertions live, which is why the Release campaigns of the (removed) auth-sharding issue 4 never saw it.
- **Where:** [`HogConservationInvariant::check`](hogd/hogdb/invariant_hog_conservation.cc#L27-L34) accumulates `total_supply_change_` from that shard's own item changes only; [`dbsched_commit_snapshot`](hogd/hogdb/rpc_from_sched.cc#L229) arms it under `if constexpr (is_debug_build())`, and [`commit_block`](hogd/hogdb/rpc_from_sched.cc#L177) turns a failure into `HogDBCode::kInternal`.

**What:** a 2-shard `launch-multi.sh` run reaches block 0, both shards commit it, and both then fail the hog-conservation invariant — **with exactly equal and opposite sums**:

```
hog11 (shard 0)  E invariant_hog_conservation.cc:31  total_supply_change_ is not 0:  312125074485502500
hog2  (shard 1)  E invariant_hog_conservation.cc:31  total_supply_change_ is not 0: -312125074485502500
                 E rpc_from_sched.cc:178             invariants check failed!!!
```

That the two sum to **zero** is the whole diagnosis. The invariant asks "did total hog supply change?", which is a property of the **whole database**; with N shards it is evaluated N times, once per shard, over that shard's slice. A transfer whose payer is owned by shard 1 and whose payee is owned by shard 0 is a credit with no matching debit on shard 0 and a debit with no matching credit on shard 1. Each shard is individually "wrong"; together they conserve exactly. Genesis is simply the first block that moves hogs across the shard boundary.

**Why it kills the process — the #6 CHECK, reached a second way.** `check_invariants()` false → `commit_block` returns `kInternal` → `dbsched_commit_snapshot` answers `DbSchedResultError` → the mempool asserts on that result:

```
mempool_exec:PANIC: rpc_from_coord.cc:1296 (TID 2072177) CHECK FAILED:
  db_result->result() == DbSchedResult::DbSchedResultOK
  [DbSchedResultError != DbSchedResultOK]
  function hogd::MempoolCoordinatorRpc::commit_block(const PendingBlock&)
```

Byte-identical to the panic in #6, and it lands in the same `commit_block`. #6's cause was ENOSPC; this one is the invariant. **Both arrive as `DbSchedResultError`, and the mempool cannot tell them apart** — which is the real lesson: `commit_block`'s `CHECK_EQ` treats every db-side failure as impossible. The hogdb then dies too, on `scheddb_hogs_transferred: RPC connection closed before reply`, because the mempool it was answering is gone.

**Why no earlier campaign hit it.** The check is `if constexpr (is_debug_build())` ([compiler.h:84](hogd/base/compiler.h#L84)), so a true Release build never arms it — the clean 2-shard run of 79M txs recorded in removed-issue 4 was Release. It fires now because this tree's Release builds had **assertions live by accident**; see the build note below.

### Build note: Release builds in this tree were not `NDEBUG`

`external/crc32c/third_party/benchmark/test/CMakeLists.txt:13` does `add_definitions(-UNDEBUG)` for every non-Debug build. It lands in the top-level directory's `COMPILE_OPTIONS`, i.e. **after** `CMAKE_CXX_FLAGS_RELEASE` on the command line, so it wins:

```
CXX_FLAGS = -Wall -march=native -Wno-interference-size -DNDEBUG -O3 -std=gnu++23 -fPIC -UNDEBUG
```

So `assert`, `x23assert` and `is_debug_build()` were all live in "Release". Worked around for the benchmark campaign with one `add_compile_options($<$<NOT:$<CONFIG:Debug>>:-DNDEBUG>)` after `add_subdirectory(external/crc32c)` in the top-level `CMakeLists.txt` — **uncommitted, and worth a proper decision**: turning `CRC32C_BUILD_BENCHMARKS` off instead breaks `x23/tests`' `bench` target, which links `benchmark::benchmark` from that same vendored copy. Note this also means every performance number measured from this build directory before 2026-08-10 was assertion-enabled.

### Fix options

1. **Make the invariant shard-aware.** It needs the cross-shard flows to net out — either exclude changes whose counterparty is not shard-local, or sum the per-shard `total_supply_change_` values and check the total once, which is what the block hash already does (`3e2d2597` sums shard partial hashes into the whole-db hash). The second mirrors existing precedent and is the only version that still catches real supply corruption.
2. **Do not let a db error be a `CHECK` in the mempool** (issue 0, again). `commit_block` should log the `DbSchedResult` and fail the block, not assert. Two unrelated defects have now reached the process through this one line.

**Repro:** any build with assertions live; `hog.conf` → `hogdb_servers=("hog11" "hog2")`; `./scripts/launch-multi.sh`. Dies at block 0, within ~10 s of the mempool reporting initialization complete.

**Artifacts:** `/home/hog12/vinita/shard-bench/evidence/s2-r1-genesis-invariant/` on NFS — both hogdb error extracts, the full mempool log with the panic, and the core listing. Core `hogdb.5200.core` (13 GB) on hog11 `/tmp/vinita`, tmpfs, will not survive a reboot.

---

## 6. hogdb fills its disk, and the mempool's `CHECK` on the error turns that into a heap-corruption mirage

- **Status:** **ROOT-CAUSED 2026-08-07; the operational trigger is fixed and the
  code defect is OPEN.** Presented for two days as intermittent heap corruption
  (`free(): invalid pointer` in `commit_block`'s unwind path at 200k-tx blocks,
  3 crashes in 5 runs). There was never an exception and never any heap
  corruption. Pre-existing on `main`, not from the sharding work, and not
  sharding-specific — every occurrence was `--num-db-shards 1`.
- **Where:** [`MempoolCoordinatorRpc::commit_block`](hogd/mempool/rpc_from_coord.cc#L1258-L1299)
  `CHECK`s the db result; [`DiskWriter`](hogd/ramdb/disk_writer.cc#L171) is where
  the real failure originates.

**The chain.** `launch-multi.sh` pointed `--db-path` at `/tmp/vinita/hogdb.data`,
a 189 GB tmpfs. A 1-shard run at `--txs-per-block 100000` writes ~141 GB of
retained WAL, fills it, and hogdb hits ENOSPC. `DiskWriter` warns without the
errno, calls `signal_ioerror()`, and `dbsched_commit_snapshot` starts returning
`DbSchedResultError`. The mempool `CHECK`s that result, and under `NDEBUG` the
`CHECK` is `std::unreachable()` — see issue 0. Rebuilding with assertions on
turned two days of garbage into one line:

```
mempool_exec:PANIC: rpc_from_coord.cc:1296 CHECK FAILED:
  db_result->result() == DbSchedResult::DbSchedResultOK
  [DbSchedResultError != DbSchedResultOK]
```

**Why every anomaly was a red herring.** Only 1 shard, because one hogdb absorbs
every byte (2 shards split the writes and never reach the ceiling). Only
`--txs-per-block 100000`, because 200k-tx blocks retain far more WAL. A narrow
batch band (728-758), because that is where cumulative writes hit the ceiling.
Intermittent ~60%, because leftover cores (10 GB each) in the same tmpfs move the
ceiling. Clean under ASan, because different throughput never reached it.

**Fixed operationally:** `hogdb_data_dir=/var/tmp/$(whoami)/hogdb.data` in
`scripts/launch-multi.sh`. Multiple 1-shard runs at the exact repro config now
pass through the crash band having written the same ~139 GB, with no cores.

**Still open:**

1. **The `CHECK` itself** — see issue 0. The mechanism is untouched; any future db
   error still kills the mempool with no diagnostic under `NDEBUG`. Three
   different causes have now produced that same fatal `DbSchedResultError`:
   ENOSPC (here), the per-shard invariant (issue 7), and a too-small
   `--ram-bytes`.
2. **[`disk_writer.cc:132,171`](hogd/ramdb/disk_writer.cc#L171) should log the
   errno.** The sibling calls at :157 and :163 print `f.error()`; these two do
   not, which is why "couldn't sync buffer to disk" never said `ENOSPC`. One-line
   fix, large diagnostic value.
3. **Nobody budgets disk for a long run.** Every service writes into `$logdir`
   without bound; only hogdb's data dir has been moved off tmpfs. Same shape as
   issue 8.

**A real, separate bug this chase did find (fixed).** `commit_block` destroyed a
still-running `Async` — `~Async` calls `callee_.destroy()` unconditionally
([async.h:555](x23/x23/async.h#L555)) and the debug-only panic is `#ifndef
NDEBUG`, so Release destroyed the live frame and the in-flight RPC completed into
freed memory. It was the lone multi-`Async` site in the file that did not drain
first. Also `commit_snapshot` took `ConsensusValue` **by value**, so the copy
lived on the CPU stack rather than the coroutine frame and its cleanup ran
against a stale pointer after a suspension. Both are fixed; neither was the cause
of this issue's crashes.

**Artifacts:** `/home/hog12/vinita/shard-bench/assert-build-runs/` and
`issue6-verify/`.

---

## 3. chuckd `panic: context canceled` on experiment teardown — 2-shard runs crash on exit (not graceful)

- **Status:** OPEN (found 2026-07-23). Lives in **groundhog-go (Go chuckd), not hogd C++.** Non-corrupting (experiment already completed all work) but it IS a crash-on-exit, not a clean shutdown — and it does not occur on unsharded runs.
- **Where:** [`UnvettedSubmitter.Start.func2`](groundhog-go/chuckd/system/coordinator/submit.go#L108) → [`must.NoError`](groundhog-go/lib-go/must/must.go#L36).

**What:** at experiment end, `coordinator_loadgen` sees `WaitWorkTxs` return `ok=false` (all 1000 batches served) → `close(stop)` → `coordinator.Stop()` cancels the context ([system.go:259-263](groundhog-go/chuckd/system/coordinator_loadgen/system.go#L259)). The background unvetted-submit goroutine had just pulled a batch off `x.parlot` and is **inside** `sched.Schedulercoord_submit_unvetted(ctx, txs)`; the cancel races in, the RPC returns `context.Canceled`, and `must.NoError(ctx, err)` panics on *any* non-nil error → whole chuckd process aborts (exit via panic, goroutine dump). The goroutine has a `case <-ctx.Done(): return` but loses the race because it was already in the RPC call.

**Why sharded shows it and unsharded does not:** the race window scales with `submit_unvetted` latency. Unsharded returns near-instantly; sharded fans the batch out to **both DB shards** — measured `submit_unvetted` io ≈ **120 ms** — so the goroutine spends far longer inside the RPC and is much more likely to be mid-call when shutdown cancels the context. A pre-existing harness race that sharding's latency exposes. (Not proven that unsharded is 100% immune — but this is the mechanism, and it matches the observation that unsharded exits are never mistaken for crashes.)

**Fix direction (groundhog-go):** make `must.NoError` (or this call site) tolerate `context.Canceled`/`context.DeadlineExceeded` during shutdown — e.g. check `ctx.Err()` before treating the RPC error as fatal, or have the submit goroutine drain/return on cancel before the RPC can report it. Separate from the hogd C++ fix in #2.

**Artifact:** `hog4:/tmp/vinita/chuckd.dev.log` — `panic: context canceled` block after `coordinator/stop.go` lines, at ~2026-07-23T16:49:35 local.

---

## 2. the coordinator connection drops mid-run and an unguarded `co_await` used to kill the mempool

- **Status:** **crash FIXED and validated on-cluster 2026-07-24; the underlying
  connection drop is OPEN and now non-fatal.** Found on 2-shard runs crashing
  mid-load around block 396. Distinct from the (removed) issue 1 genesis crash —
  this happens deep into a healthy run, not at block 0.
- **Where:** the detached "launch vetted notify" coroutine at
  [rpc_from_coord.cc:310-331](hogd/mempool/rpc_from_coord.cc#L310-L331).

**The fixed crash.** An unguarded `co_await
coord.coord_client_->coordscheduler_notify_vetted(response)` in an
`x23::Detached` coroutine. When the stream fails, `RpcStreamImpl::fail()`
([rpc.cc:38-50](x23/x23/rpc.cc#L38-L50)) hands every in-flight call an empty
buffer, deserializing it throws `XdrBadInput`, and an exception escaping a
detached coroutine `terminate()`s the process. `git blame` puts it at `ad9e1d32`,
which is on `main`; it never fires there because the coordinator connection does
not break. A 37 GB core confirmed the mechanism exactly: one faulting thread,
`reader()` → `fail()` → the detached lambda → `__cxa_call_terminate`, and no
shard-path frame anywhere.

**The guard fired and worked.** Post-fix run: `E rpc_from_coord.cc:338
coordscheduler_notify_vetted failed: … input too short. Dropping vetted-tx
notification.` at block 393, then a normal `schedulercoord_nominate ret`. The
run completed 1000/1000 work batches, 393 blocks, ~39.9M txs at ~18,058 tx/s,
with no conservation/invariant/CHECK failures and both shards non-empty.

**What is still open: why the connection drops.** The 252 MB record length is
*legitimate*, not corruption — `ParallelBatch` concatenates every serialized
object into one byte-vector ([xdr_util.h:203](hogd/base/xdr_util.h#L203)), so
~500k txs is ~250 MB, and `read(..., full=true)` returning 11 MB means the fd was
closed mid-transfer rather than a short socket read. The mempool was not falling
behind (steady nominate/commit pairs to the last block) and did not cause it, so
it originates on the chuckd side or in mempool↔chuckd backpressure. The one
correlated event is a chuckd single-validator SCP stall (`no scp providers among
known validators`) in the same 1-2 s window. **The block-393 drop was
shutdown-induced** (chuckd closing mid-`notify_vetted`), so a purely spontaneous
mid-run drop still has not been isolated.

**The lesson generalised, and it is now load-bearing.** Issue 10 is this same
defect at a third call site: `handle_transfer` is a detached coroutine whose
`co_await`s on the db throw when that connection dies, and it never got the
guard. **Audit every detached `co_await` on a peer connection**, rather than
fixing them one crash at a time.

**Repro:** `hog.conf` → `hogdb_servers=("hog11" "hog2")`; idle cluster;
`./scripts/launch-multi.sh`; wait ~35-40 min / ~block 395.

**Artifacts: LOST 2026-08-06** — hog10 rebooted and `/tmp` is tmpfs, taking the
37 GB core and its log. Preserve future artifacts on NFS
(`/home/hog12/vinita/...`), not `/tmp/vinita`.

**Teardown gotcha:** `pkill -f 'scripts/launch-multi.sh'` self-matches the
wrapper shell — use the bracket trick (`pgrep -f 'launch[-]multi\.sh'`).

---

