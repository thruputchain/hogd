# Issues

A running log of known issues. Newest first. Each entry: status, where, what, why, repro, artifacts.

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

- **Status:** OPEN (found 2026-08-10 @ `97c4f6cc`, while setting up the sharded-vs-unsharded benchmark). **Not sharding-correctness** — the data is fine and the shards agree; the *check* is wrong. Only bites builds with assertions live, which is why the Release campaigns in #4 never saw it.
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

**Why no earlier campaign hit it.** The check is `if constexpr (is_debug_build())` ([compiler.h:84](hogd/base/compiler.h#L84)), so a true Release build never arms it — #4's clean 2-shard run of 79M txs was Release. It fires now because this tree's Release builds had **assertions live by accident**; see the build note below.

### Build note: Release builds in this tree were not `NDEBUG`

`external/crc32c/third_party/benchmark/test/CMakeLists.txt:13` does `add_definitions(-UNDEBUG)` for every non-Debug build. It lands in the top-level directory's `COMPILE_OPTIONS`, i.e. **after** `CMAKE_CXX_FLAGS_RELEASE` on the command line, so it wins:

```
CXX_FLAGS = -Wall -march=native -Wno-interference-size -DNDEBUG -O3 -std=gnu++23 -fPIC -UNDEBUG
```

So `assert`, `x23assert` and `is_debug_build()` were all live in "Release". Worked around for the benchmark campaign with one `add_compile_options($<$<NOT:$<CONFIG:Debug>>:-DNDEBUG>)` after `add_subdirectory(external/crc32c)` in the top-level `CMakeLists.txt` — **uncommitted, and worth a proper decision**: turning `CRC32C_BUILD_BENCHMARKS` off instead breaks `x23/tests`' `bench` target, which links `benchmark::benchmark` from that same vendored copy. Note this also means every performance number measured from this build directory before 2026-08-10 was assertion-enabled.

### Fix options

1. **Make the invariant shard-aware.** It needs the cross-shard flows to net out — either exclude changes whose counterparty is not shard-local, or sum the per-shard `total_supply_change_` values and check the total once, which is what the block hash already does (`3e2d2597` sums shard partial hashes into the whole-db hash). The second mirrors existing precedent and is the only version that still catches real supply corruption.
2. **Do not let a db error be a `CHECK` in the mempool** (issue #1 rec 3, again). `commit_block` should log the `DbSchedResult` and fail the block, not assert. Two unrelated defects have now reached the process through this one line.

**Repro:** any build with assertions live; `hog.conf` → `hogdb_servers=("hog11" "hog2")`; `./scripts/launch-multi.sh`. Dies at block 0, within ~10 s of the mempool reporting initialization complete.

**Artifacts:** `/home/hog12/vinita/shard-bench/evidence/s2-r1-genesis-invariant/` on NFS — both hogdb error extracts, the full mempool log with the panic, and the core listing. Core `hogdb.5200.core` (13 GB) on hog11 `/tmp/vinita`, tmpfs, will not survive a reboot.

---

## 6. mempool `free(): invalid pointer` in `commit_block`'s unwind path — heap corruption at 200k-tx blocks

- **Status:** **ROOT-CAUSED 2026-08-07. Not a heap-corruption bug at all — hogdb ran out of disk and the mempool's `CHECK` on the error result is UB in Release.** See "Actual root cause" immediately below; everything after it is the two-day chase that got there, kept for the lessons. **The `commit_block` drain fix below did NOT fix it** — verification run 1 (2026-08-07 17:55, post-fix) crashed at work batch **728**, same band, with a *different* signature: **SIGSEGV in `__cxa_begin_catch`**, same `commit_block.actor.cold` frame, same RPC reader thread, no log line and no `free()` message. The fix is still correct as far as it goes (it removes a real UAF and a real stale-cleanup hazard) but the defect is **upstream of `commit_block`** — see "Run 1 post-fix" below. **Pre-existing, NOT from the rebase** — reproduced on `b5b88e1f` (pre-rebase, flat `ShardCard`, without `b748e78e`) with a byte-identical stack. Intermittent: **3 crashes in 5 runs**. Not sharding-specific (every occurrence was `--num-db-shards 1`).
- **Where:** [`MempoolCoordinatorRpc::commit_block`](hogd/mempool/rpc_from_coord.cc#L1258-L1299), the `[clone .cold]` (exception-unwind) path, on the RPC reader thread.

### Actual root cause (confirmed 2026-08-07, assert-enabled build)

Rebuilding with `NDEBUG` off turned two days of unreadable garbage into one line:

```
mempool_exec:PANIC: rpc_from_coord.cc:1296 (TID 8074) CHECK FAILED:
  db_result->result() == DbSchedResult::DbSchedResultOK
  [DbSchedResultError != DbSchedResultOK]
  function hogd::MempoolCoordinatorRpc::commit_block(const PendingBlock&)
```

**There was never an exception and never any heap corruption.** The db returned `DbSchedResultError`, the `CHECK_EQ` on it failed, and under `-DNDEBUG` `CHECK` → `x23::panic` → **`std::unreachable()`** ([debug.h:191-196](x23/x23/debug.h#L191)) — undefined behaviour at `-O3`. The "crashes" were whatever the optimiser left behind: a `free()` of `commit_block.actor`'s own code address in one run, a jump into a catch handler with a nonsense `_Unwind_Exception*` (`0x195a`) in the next. This is precisely the latent hazard recorded as recommendation 3 of issue #1 ("every `CHECK` in a Release build is a latent UB SIGSEGV; this one was just the first to fire") — now demonstrated.

**Why the db failed: the hogdb data directory is on tmpfs and it filled up.** `launch-multi.sh` passes `--db-path /tmp/vinita/hogdb.data`, and `/tmp` on hog11 is a 189 GB tmpfs. At the crash, hogdb had written **141 GB** into it:

```
hogdb.log:  W disk_writer.cc:171  couldn't sync buffer to disk
            disk stats: bytes enqueued 141GiB, bytes written 140.5GiB, write bw 113.7MiB/s -> 0B/s
hog11:      tmpfs 189G 189G 0 100% /tmp        141G /tmp/vinita/hogdb.data
```

ENOSPC → [`DiskWriter`](hogd/ramdb/disk_writer.cc#L171) warns and calls `signal_ioerror()`, the db enters its I/O-error state, and `dbsched_commit_snapshot` starts returning `DbSchedResultError`.

**This retires every anomaly in the entry below:**

| observation | explanation |
|---|---|
| only `--num-db-shards 1` | one hogdb absorbs every byte; 2 shards split the writes and never reach the ceiling. Nothing to do with sharding correctness. |
| only `--txs-per-block 100000` | the 200k-tx blocks this config packs produce far more retained WAL (`file N.log needed for recovery; retaining`) than the 1M-txs/block runs. |
| narrow batch band (734-758, 728, 787) | the batch at which cumulative writes hit the tmpfs ceiling. |
| intermittent ~60% | depends on free space at start — leftover cores (10 GB each!) and old logs in the same tmpfs move the ceiling. |
| no log line at all | `panic` is `std::unreachable()` under NDEBUG, so the `CHECK` printed nothing; hogdb's one-line warning omits the errno (see fix 2) and was buried in 9.9 MB of debug output. |
| ASan run clean to 999 | different throughput and retention, so it did not reach the ceiling — not a sanitizer-visible bug in the first place. |

### Fixes this calls for

1. **`CHECK` must abort with a diagnostic under `NDEBUG`, not `std::unreachable()`** (issue #1, rec 3). This is the single change that would have turned a two-day forensic exercise into a one-line log. It is a repo-wide behaviour change to [`x23::panic`](x23/x23/debug.h#L191) and should be a deliberate decision, not smuggled in with a bug fix — flagging it rather than doing it.
2. **[`disk_writer.cc:132,171`](hogd/ramdb/disk_writer.cc#L171) should log the errno.** The sibling calls at :157 and :163 print `f.error()`; these two do not, which is why "couldn't sync buffer to disk" never said `ENOSPC`. One-line fix, large diagnostic value.
3. **Operational: do not put a long benchmark's hogdb data on tmpfs.** 1-shard @ 100k txs/block writes ~141 GB. Either point `--db-path` at real disk, cap the run, or budget the tmpfs (and stop leaving 10 GB cores in it).
4. **Not a fix for this issue, but keep it:** the `commit_block` drain change below fixes a *different*, real defect that this investigation reproduced deterministically — see "Bare-shape reproduction" below.

### Bare-shape reproduction (the one real bug this chase did find)

[`x23/tests/two_call_await_race.cc`](x23/tests/two_call_await_race.cc) recreates `commit_block`'s shape — two RPCs launched on two connections, awaited in sequence, with the first-awaited connection poisoned mid-call — and it settles the `commit_block` question in seconds rather than a 25-minute cluster run:

| shape | result |
|---|---|
| bare (pre-fix: `co_await a; co_await b;`) | **PANIC within 1-12 s**: `Premature destruction of a running Awaitable or Async [flags = 0x14]` |
| drained (the fix) | 640k rounds, 1,228 injected exceptions, 50 s, zero panics |

The panic's stack is the production stack — `~Awaitable<Vec<unsigned char>>` ← `RpcStream::send_call.actor` ← `<coroutine>.actor.cold` ← `RpcStreamImpl::reader.actor` ← `ThreadPool::Worker::work` — and `flags = 0x14` matches the value in `commit_block`'s frame in the core. So abandoning a sibling Async on the unwind path is real and reachable; it simply was not what killed the mempool here. Run with `-b` for the bare shape; not registered as a ctest, matching x23's other race soak tests.


**What:** the mempool aborts mid-run with glibc's heap-corruption message. Not OOM — the host had >300 GB free.

```
Program terminated with signal SIGABRT — free(): invalid pointer
#6  x23::Vec<unsigned char, 128>::realloc(unsigned long)
#7  hogd::MempoolCoordinatorRpc::commit_block(...) [clone .actor] [clone .cold]
#8  x23::detail::RpcStreamImpl::reader(...)
#9  x23::ThreadPool::Worker::work()
```

**Reading the frames.** `Vec`'s second template parameter is `MaxSize`, not an inline capacity, so `Vec<unsigned char, 128>` is **`Upgrade`** ([`typedef opaque Upgrade<128>`](protocol/consensus.x#L10)), carried as `Upgrade upgrades<6>` inside `ConsensusValue` — it is *not* `x23::Buf` (`Buf = Vec<uint8_t>` uses the default MaxSize). And `realloc(0)` is the **destructor** ([vec.h:366](x23/x23/vec.h#L366) `~Vec() { realloc(0); }`), which skips allocation and goes straight to `a_.deallocate(b_.begin, ...)`. So this is an `Upgrade` being **destroyed with a `b_.begin` the allocator rejects**, during exception unwinding — not a buffer outgrowing itself.

Neither side ever populates the field: the mempool sets `.upgrades = {}` at both construction sites ([rpc_from_coord.cc:1466, :2113](hogd/mempool/rpc_from_coord.cc#L1466)), and chuckd only *compares* upgrades (`upgradesEqual`, `chuckd/util/protoutil/value.go`). So the corrupt `Upgrade` reaches the destructor via a `ConsensusValue` that is itself already damaged, or is destroyed twice. **Resolved 2026-08-07: neither.** No `Upgrade` was ever really destroyed — the cleanup ran over garbage. See below.

**Root cause: `commit_block` was the one place in this file that let an exception unwind out of a `co_await`.** It launches two RPCs and then awaits them bare:

```cpp
auto commit_snapshot_handle = commit_snapshot(cache.block_id, cache.db_tip.value);
auto event_commit_handle    = eventsched_commit_block(cache.block_id);
DbBlockResult result = co_await commit_snapshot_handle;   // throws -> unwind
```

Two defects fire together on that unwind, and both are cheap to remove:

1. **A still-running `Async` is destroyed.** `~Async` calls `callee_.destroy()` unconditionally ([async.h:555](x23/x23/async.h#L555)); the header states you must get an Async's result before destroying it, and `~Awaitable` even panics on it — but only `#ifndef NDEBUG` ([async.h:255-265](x23/x23/async.h#L255-L265)), so Release destroys the live frame silently and the in-flight RPC completes into freed memory. Every *other* multi-Async site in this file already drains first (`finish_all_executors`, `propose_batch`, `prepare_validation_chunk`, the validate decode pair, both abandon nodes) — `commit_block` was the lone holdout, and `ShardedClient::drain_awaits` exists for exactly this reason.
2. **The `ConsensusValue` argument copy is destroyed by a stack-relative cleanup after a suspension.** `commit_snapshot` took `ConsensusValue` **by value**, so `commit_block` materialized the copy as a temporary on its own *CPU stack* (`rsp+0x1e0`), not in the coroutine frame. An actor re-entered after a suspension has a fresh stack frame, so running that cleanup post-resume frees whatever the stale pointer register happens to hold.

**How the core proves (2).** The freed pointer was `0x55c77e5fcf90` = **`commit_block.actor` itself**, read as `Vec::b_.begin` from offset 0 of a live coroutine frame: `%r12` (the cleanup's object pointer) equalled `%rbx` (the actor's frame pointer, `0x7f1f48222500` — a live `0x8b0` malloc chunk whose first two words are commit_block's `.actor`/`.destroy`). The two landing pads in `.actor.cold` destroy exactly a `Vec<Upgrade,6>` at `rsp+0x1e0` and one `Upgrade` at `%r12` — i.e. `x23::Vec`'s `Vec(from_range)` `catch { realloc(0); throw; }` ([vec.h:344-353](x23/x23/vec.h#L344-L353)) around the inlined `ConsensusValue` copy. Frame `+0x40`/`+0x48` held both live `Async` handles and the promise flags were `kRunning|kAwaiting` (0x14), so the body was well past launching both RPCs; frame #8 is `RpcStreamImpl::reader`, so the exception arrived with an RPC completion.

**Fix (uncommitted, on `vp/sharding-v2-db-abstraction`):**
- `commit_block` drains both handles into `std::optional`s, keeps the first exception, logs it (the crash left *no* log line), rethrows once, then `CHECK_EQ`s the results — the same shape as the abandon nodes a few hundred lines below.
- `MempoolCoordinatorRpc::commit_snapshot` and `ShardedDbSched::commit_snapshot` now take `ConsensusValue const &`, so no by-value temporary exists in a coroutine that suspends. The one copy the wire needs is made by the generated RPC stub, in that stub's own coroutine frame (frame-resident params are destroyed by the frame's `.destroy`, not by a stack-relative landing pad).
- Builds clean; `mempool_test` 72/72. **No unit test:** the mempool suite has no way to make a db/event RPC throw mid-commit, and the `ShardedDbSched` tests only cover `combine_block_results`. An injectable-failure harness for the commit path is the missing coverage.
- **Still open:** *what* threw. The exception object was not recoverable from the core, and nothing logged.

### Run 1 post-fix (2026-08-07 17:31–17:55, batch 728) — the defect is above `commit_block`

Same function, same reader thread, new signature: `SIGSEGV in __cxa_begin_catch` called from `commit_block.actor.cold` (core `mempool_exec.6451.core` on hog10, matching binary preserved at `/home/hog12/vinita/issue6-verify/mempool_exec.run1-binary`, sha256 verified against the crashed process). Evidence from that core:

- **The catch handler is entered with a garbage exception pointer.** `si_addr == 0x195a` and the faulting instruction is `add (%rbx),%rcx` at `__cxa_begin_catch+0x24`, where glibc++ keeps the `_Unwind_Exception*` argument in `%rbx` (`mov %rdi,%rbx` in the prologue) and reads its `exception_class` field. So the value handed to the catch handler is **`0x195a`** — a small integer, not a pointer. (An earlier reading of `%rdi == 3` was unreliable: `%rdi` is caller-saved and the intervening `call __cxa_get_globals` clobbers it. `si_addr`/`%rbx` is the trustworthy datum.) Suggestively, `0x195a == 6490` is both the crashing thread's LWP **and** the size class of an RPC record — i.e. a leftover byte count in the register the unwinder would have used for the exception object.
- **Both child Asyncs are finished and already consumed.** `commit_block`'s frame (`0x7ff49f4a8c80`) holds `commit_snapshot_handle` at `+0x40` and `event_commit_handle` at `+0x48`; both child frames have `__resume == nullptr` (GCC's "coroutine done") and promise `flags == 0`, i.e. `await_resume`'s `flags_.exchange(0)` already ran on both. Both still hold `caller_ == commit_block`'s frame.
- **`commit_block` itself still thinks it is suspended**: its own promise flags are `0x14` (`kRunning|kAwaiting`) and `resume_at == 6`, yet it has consumed both results and has nothing left to await.
- A heap scan for references to `commit_block`'s frame finds exactly **3**: the two children's `caller_` and the parent dag-node's `Async` handle. No stale fourth party, which argues against "an older frame was freed and its address reused".

**Hypothesis tested and rejected: a stale coroutine resume index.** If the actor were re-entered with an unwritten `resume_at`, the jump table could land in a landing pad, which would explain a garbage exception pointer. It does not hold: disassembling the second await in the crashing binary shows GCC stores the index **before** authorizing the callee to resume —

```
25e3da:  movw  $0x6,0x38(%rbx)     ; resume_at = 6, written FIRST
25e3eb:  mov   0x18(%rax),%eax     ; then read the child's flags (await_ready)
25e3fd:  mov   %rbx,(%rdx)         ; then caller_ = our frame
25e40a:  lock cmpxchg %esi,0x8(%rdx) ; then publish kAwaiting
```

So `resume_at == 6` in the core is simply the last value written — `commit_block` had resumed from its second await and consumed the result (which is why child2's `flags == 0`) and had not yet reached its final suspend (which writes index 8). The frame state is *self-consistent*; what is inconsistent is the exception being delivered to it.

**Leading hypothesis (not proven):** `commit_block`'s frame is resumed **once too many** — after both awaits have completed and cleared their flags — so `await_resume` runs with `f == 0`, falls past `x23assert(f & kException)` (**compiled out under `NDEBUG`**) into the exception branch, and rethrows an `exception_ptr` whose bytes are a stale result. Everything downstream (garbage landing pad, `__cxa_begin_catch(3)`, and the earlier `free()` of a code pointer) follows from that. `commit_block` is simply the coroutine most exposed to it: it is the only place that keeps **two RPCs in flight on two different connections** (hogdb and event), so two independent reader threads can drive its completions.

**Next step: stop inferring, make it assert.** x23 already has the two checks that would name the culprit at the moment it happens, and both are disabled in this build:
- `~Awaitable` panics with a trace of the destroying site on premature destruction — `#ifndef NDEBUG` only ([async.h:255-265](x23/x23/async.h#L255-L265)).
- `await_resume`'s `x23assert(f & kException)` is exactly the condition hypothesised above ([async.h:349](x23/x23/async.h#L349)).

So the run to do next is the same repro against a build **without `-DNDEBUG`** (keep `-O2/-O3`, since the bug is timing-dependent). Caveat: like ASan, it perturbs timing and may not reproduce — but unlike ASan it fails *at the bad operation* rather than downstream, so a single hit is conclusive.

**Artifacts:** `/home/hog12/vinita/issue6-verify/run1-postfix-release/` on NFS — mempool log, metrics, chuckd tail, silent-failure scan (clean: zero conservation/invariant/CHECK/PANIC hits on all eight hosts), and `summary.txt`. Core `mempool_exec.6451.core` on hog10 tmpfs; backtrace at `/tmp/vinita/gdb_bt_6451.txt`.

**Why it surfaced now — a new workload shape, not new code.** Only the **1-shard, `--txs-per-block 100000`** configuration triggers it, which is the only one that packs **200k-tx blocks** (chuckd merges two work batches per block when it keeps up). Every prior campaign ran `--txs-per-block 1000000` and never hit it. The crash always lands in a narrow band, work batch **734–758**, right where blocks start alternating 100k/200k.

| run | code | build | outcome |
|---|---|---|---|
| 1 | post-rebase | Release | crash @ batch 756 |
| 2 | post-rebase | Release | crash @ batch 758 |
| 3 | post-rebase | **ASan** | clean to 999 |
| 4 | post-rebase | Release | clean to 999 |
| 5 | **pre-rebase `b5b88e1f`** | Release | **crash @ batch 734** |

**ASan (clang Release + `-fsanitize=address`) reported nothing at all** on any of the 8 hosts. ASan detects double-free and use-after-free deterministically *when they execute*, so silence means the bad path was not taken that run — not that ASan missed it. Run 4 (plain Release, clean) shows the non-repro is not ASan's doing either; the thing is simply intermittent. The root cause above explains the silence: the bad free is neither a double-free nor a UAF *write* that ASan poisons — it is `operator delete` on a `.text` address, reached only when a commit RPC throws.

**Consequences for benchmarking:** 1-shard @ 100k txs/block is unusable as a benchmark config (dies ~60% of the time before finishing). Everything measured at 1M txs/block is unaffected — all of those runs completed cleanly.

**Repro:** worktree or checkout at any of `b5b88e1f`/`97c4f6cc`; `hog.conf` → `hogdb_servers=("hog11")`; `scripts/launch-multi.sh` → `--txs-per-block 100000`; idle cluster; `./scripts/launch-multi.sh`; watch for the mempool dying around work batch ~750. Expect ~60% incidence.

**Artifacts (hog10, `/tmp/vinita`, tmpfs — will not survive a reboot):** cores `mempool_exec.255709.core`, `mempool_exec.256585.core` (post-rebase), `mempool_exec.5177.core` (pre-rebase, matching binary at `/home/hog12/vinita/hogd-prerebase/build/hogd/mempool/mempool_exec`); logs `preserved_crash2_mempool.log`, `preserved_prerebase_mempool.log`; all-thread backtrace of the pre-rebase core at `gdb_bt_5177.txt`.

---

## 5. genesis reports complete before its hog accounting lands — the first submitted batch is discarded for "0 hogs"

- **Status:** FIXED 2026-08-04 on `vp/sharding-v2-db-abstraction` (uncommitted). Found while bringing up `hog-devnode --num-db-shards 2`. **Pre-existing on `main` and not shard-specific** — 1 shard wins the race by luck, N shards lose it reliably.
- **Where:** [`schedcoord_load_genesis_configuration`](hogd/mempool/rpc_from_coord.h#L250-L268) force-stored `transfer_finished_ = 0`, opened the `all_shards_genesis_ready_` barrier, and returned immediately; the block-0 deltas that barrier unblocks are applied asynchronously in [`MempoolDbRpc::handle_transfer`](hogd/mempool/rpc_from_db.h#L62-L114).

**What:** genesis writes the initial hog supply to the db, and the db then *sends* the mempool a `HogsTransferred` so the scheduler can build its in-memory per-account hog state. The RPC returned between those two steps. A caller that submits as soon as it returns — hog-devnode's initialization batch, chuckd's init batch — hits a scheduler that has no hogs for any genesis account yet, and every tx is discarded:

```
scheduler: discarding tx … for account [0,…,0,2]:
  gas_limit 1e+09 exceeds all hogs available 0
  (to_schedule 0, already_scheduled 0, received 0, spent_this_block 0)
```

All three groups zero is the tell: this is *missing accounting*, not hogs sitting in the wrong group (the scheduler cycles between groups on demand). The db was always correct — instrumenting the genesis write and the hog-age read showed `genesis WROTE supply hog age on shard 1/2: 625000000000000000` while the scheduler's read returned `to_schedule=0`.

**Why the shard count decides it:** `handle_transfer` runs once per shard, serially under `scheduler_lock_`, each doing a sharded hog-age read plus a fan-out write, and [`record_shard_transfer_finished`](hogd/mempool/scheduler.h#L181-L192) only advances `transfer_finished_` once *every* shard reports. One shard completes in microseconds and beats the submit; two shards do not.

**Fix:** drop the force-store and `co_await wait_for_transfer_data(...)` after opening the barrier, so the RPC's success actually means "genesis is accounted for". Cannot hang on a shard that owns none of genesis — `BackgroundCommitWorker::commit_job` dispatches a `HogsTransferred` for every committed block even with zero deltas. Cannot deadlock — the wait holds `commit_lock_`, which `handle_transfer` never takes.

**Repro (before the fix):** `hog-devnode --dev-accounts 4 --num-db-shards 2`; fails before readiness with `bootstrap account metadata does not match recovered chain: missing auth data`, because the discarded batch never created the dev wallets. Inserting a 3 s sleep between genesis and the init batch made it pass — that was the confirmation.

**After:** ready at 1, 2 and 4 shards; 6/6 consecutive 2-shard runs clean; `--data-dir` recovery works at 2 shards; devnode 102/102, mempool 72/72, hogdb 218/218, db_shard_routing 20/20.

**Worth checking on-cluster:** chuckd submits its init batch right after genesis too, so this may account for some of what #4 attributed to auth.

---

## 4. auth server is not sharded — 2-shard runs auth-fail 100% of the load and nominate empty blocks forever

- **Status:** FIXED and **verified on-cluster 2026-08-04** (fix committed as `3001b393`). A 2-shard `launch-multi.sh` run reached its natural end (work batch 999) with `mempool_auth_failed_tx_count` **0** for the whole run, 79,490,644 txs committed across 343 non-empty blocks, shards balanced 49%/50%, and zero cores or invariant/conservation failures on any of the nine hosts. Auth logged `auth server bound 2 db shard(s)` and held connections to *both* hogdbs (vs 2-to-one/0-to-the-other before). All three gaps below are closed: [`auth_init`](hogd/auth/auth.cc#L85-L100) now orders the cards with `order_shards_by_id` and builds one `dbauth_client` per shard; [`DBAuthClient`](hogd/auth/dbauth_client.h#L38-L67) holds a `ShardedClient` and routes `lookup_auth_data` by address; `register_auth_data_updates` merges the per-shard slices via `merge_shard_key_changes` before advancing the height. Originally found 2026-07-29 @ `d6a6fc13`. **Hard liveness stall, no crash.** The latent defect was **pre-existing on `main`**; commit `97ee1d80` on this branch is what exposed it.
- **Where:** [`auth_init`](hogd/auth/auth.cc#L80-L87) binds `intro.shards()[0]`; [`DBAuthClient`](hogd/auth/dbauth_client.h#L41-L42) holds one `dbauth_client_t` (its own comment says *"Sharded DB is not yet implemented."*); [`AuthServer::register_auth_data_updates`](hogd/auth/auth_server.cc#L83-L86) carries a `TODO(geoff)` saying the height sequencing "will break if there's a sharded db".

**What:** the auth server reads **all** account auth/key state from a **single** hogdb. `auth.cc` takes `ShardCard &shard = intro.shards()[0]` and builds one `dbauth_client`, and `subscribe_to_auth_data_changes` subscribes to that one shard only. The mempool's intro already returns *every* shard card ([rpc_from_auth.h:43-46](hogd/mempool/rpc_from_auth.h#L43-L46)) — auth just ignores all but the first. Note `shards()[0]` is **connection-arrival order, not shard id**: in the observed run auth bound to shard **1**. Confirmed on-cluster: auth host hog8 held **2 TCP connections to hog2 (shard 1) and 0 to hog11 (shard 0)**.

**Why it is only now fatal:** that `shards()[0]` line predates this branch (last touched on `main` 2026-07-13, `771f0657`, a mechanical refactor). It was harmless while genesis was loaded **replicated in full on every shard**, so any single shard held all genesis-seeded auth data. `97ee1d80` ("sharding: load genesis shard-owner-only with a global block hash", 2026-07-28) removed that accidental safety net. This is why the 2026-07-27 run reached 436 blocks / 39.9M txs and this one produces no throughput number at all.

**Symptom — looks like healthy consensus, is a stall.** chuckd commits init tx batch 0 (10002 txs) at height 1, then submits init batch 1 (10000 txs), which is **100% rejected at authentication**, so nothing is ever nominatable. chuckd then nominates **empty** blocks forever: `NOMINATED/COMMITTED: height N; tx_count 0` plus `commit hook: continue nominating (10000 uncommitted)` — **16,945 empty blocks in ~28 min**, height climbing fast, both hogdb shards committing with distinct `db_hash`es, mempool alive, and **zero** crash/CHECK/invariant/conservation/`Not Found` lines on any of the nine hosts. The run never reaches the *work* phase, so `work batch` never appears in the chuckd log.

**Detection:** no log line reveals this — the mempool metrics endpoint does. `curl hog10:8075/metrics`:

```
mempool_submitted_unvetted_tx_count  20002   # both batches arrived
mempool_accepted_unvetted_tx_count   10002   # only batch 0 accepted
mempool_auth_failed_tx_count         10000   # ALL of batch 1 rejected by auth
mempool_nomination_nominated_tx_count 10002
```

A fast-climbing block height with `tx_count 0` means an auth/vetting reject loop, not progress. **Check `mempool_auth_failed_tx_count` before believing a sharded run is healthy.**

**Not yet explained:** why 100% and not the ~50% address routing would suggest. Consistent with a single signer/authority address owned by the unconnected shard; unverified.

### Three distinct gaps in the auth→db path

1. **Read routing (the stall).** `lookup_auth_data(height, address)` must go to `shard_of_address(address, total_shards)`, not to one fixed shard.
2. **Subscription fan-out.** `dbauth_intro` must go to *every* shard; each returns its own `current_height`, and per [sharding.h](hogd/db_shard_routing/sharding.h#L48-L50) shard tips must agree — so divergent/partial heights are fail-stop (mirroring `5c584c43`).
3. **Key-change aggregation (the `TODO(geoff)`).** With N shards, each block delivers **N** `authdb_forward_key_changes(height, …)` calls. `register_auth_data_updates` advances the height on the first and discards the rest as `"Received stale update for block height … ignoring"` — so **N-1 shards' key changes are silently dropped**, and the `mempoolauth_auth_data_changed` forward fires N times per height. Needs a per-height barrier that merges all N batches before applying and advancing once (mirroring `34330f31`).

**Repro:** `hog.conf` → `hogdb_servers=("hog11" "hog2")`; idle cluster; `./scripts/launch-multi.sh`; within ~1 min `curl $(hog_ip hog10):8075/metrics | grep auth_failed`.

**Artifacts:** `mempool_metrics_stall.txt` (scratchpad); per-host logs in `/tmp/vinita/` (chuckd 2.2 GB at `-l debug`; mempool only 3 MB — two lines per block, nothing else).

**Teardown gotcha:** `launch-multi.sh` ends in `sleep infinity` and its `EXIT/INT/TERM` trap did **not** fire on `SIGTERM` from `timeout`; the script had to be `kill -9`ed and the per-host `killall` run by hand. `pgrep -cf '<pattern>'` over ssh **self-matches** the ssh command line — use `pgrep -a -x` on exact binary names to verify teardown.

---

## 3. chuckd `panic: context canceled` on experiment teardown — 2-shard runs crash on exit (not graceful)

- **Status:** OPEN (found 2026-07-23). Lives in **groundhog-go (Go chuckd), not hogd C++.** Non-corrupting (experiment already completed all work) but it IS a crash-on-exit, not a clean shutdown — and it does not occur on unsharded runs.
- **Where:** [`UnvettedSubmitter.Start.func2`](groundhog-go/chuckd/system/coordinator/submit.go#L108) → [`must.NoError`](groundhog-go/lib-go/must/must.go#L36).

**What:** at experiment end, `coordinator_loadgen` sees `WaitWorkTxs` return `ok=false` (all 1000 batches served) → `close(stop)` → `coordinator.Stop()` cancels the context ([system.go:259-263](groundhog-go/chuckd/system/coordinator_loadgen/system.go#L259)). The background unvetted-submit goroutine had just pulled a batch off `x.parlot` and is **inside** `sched.Schedulercoord_submit_unvetted(ctx, txs)`; the cancel races in, the RPC returns `context.Canceled`, and `must.NoError(ctx, err)` panics on *any* non-nil error → whole chuckd process aborts (exit via panic, goroutine dump). The goroutine has a `case <-ctx.Done(): return` but loses the race because it was already in the RPC call.

**Why sharded shows it and unsharded does not:** the race window scales with `submit_unvetted` latency. Unsharded returns near-instantly; sharded fans the batch out to **both DB shards** — measured `submit_unvetted` io ≈ **120 ms** — so the goroutine spends far longer inside the RPC and is much more likely to be mid-call when shutdown cancels the context. A pre-existing harness race that sharding's latency exposes. (Not proven that unsharded is 100% immune — but this is the mechanism, and it matches the observation that unsharded exits are never mistaken for crashes.)

**Fix direction (groundhog-go):** make `must.NoError` (or this call site) tolerate `context.Canceled`/`context.DeadlineExceeded` during shutdown — e.g. check `ctx.Err()` before treating the RPC error as fatal, or have the submit goroutine drain/return on cancel before the RPC can report it. Separate from the hogd C++ fix in #2.

**Artifact:** `hog4:/tmp/vinita/chuckd.dev.log` — `panic: context canceled` block after `coordinator/stop.go` lines, at ~2026-07-23T16:49:35 local.

---

## 2. mempool `terminate()` via `XdrBadInput` on coordinator RPC — 2-shard runs crash mid-load (~block 396)

- **Status:** Bug 1 (crash) FIXED + **VALIDATED ON-CLUSTER 2026-07-24** (with the sharding refactor). The guard **fired and worked**: `E rpc_from_coord.cc:338 coordscheduler_notify_vetted failed: … x23::Deserializer: input too short. Dropping vetted-tx notification.` at block 393, immediately followed by `RpcStreamImpl: read(fd 20, n 252000212) returned 9006556 bytes` and then a normal `schedulercoord_nominate ret` — the exact short-read + in-flight-`notify_vetted` condition that previously `terminate()`d the mempool and dumped a 37 GB core was caught, and the mempool **survived**. Full experiment then completed (1000/1000 work batches, 393 blocks, ~39.9M txs @ ~18,058 tx/s); 0 conservation/invariant/CHECK/NotFound/hog-age failures across mempool/hogdb×2/hogexec/auth/event; both shards non-empty (shard0 64%, shard1 5% mem-items). Bug 2 (why the coord short-read happens) still open but now non-fatal; the block-393 drop was shutdown-induced (chuckd closing mid-`notify_vetted`), so a purely *spontaneous* mid-run drop still hasn't been isolated — the guard handled the identical fatal condition regardless. Distinct from #1 (genesis crash) — happens deep into a healthy run, not block 0.

**Re-confirmed 2026-07-27:** clean rerun on a hogci-free cluster reached **work batch 1000** with the mempool **ALIVE the whole time** — guard fired again near the end (`Dropping vetted-tx notification` + `read(fd 23, n 252000212) returned 13046476 bytes` → normal `nominate ret`), **zero cores dated today**, no crash signatures. So: the mempool no longer crashes before completing the 1000 work batches. (Bug 3 did not recur this run — it's a race.)

**Also observed:** the CI-collision hazard is real — an earlier attempt was SIGKILLed at block 68 by a concurrent `hogci` benchmark's `killall` (no core, no crash text, "input too short" cascade). Verify hosts are hogci-free before running; `killall` as vinita only touches vinita's procs.

**[SUPERSEDED by the 2026-07-24 validated run above — kept for history.]** **On-cluster result 2026-07-23 (post-fix rerun) — full experiment ran to its natural end, but the fatal path was NOT re-tested and the exit was NOT clean:**
- The run reached its **natural end**: chuckd's loadgen served all **1000 configured work batches** (`WaitWorkTxs` → `ok=false` at batch 1000), across **436 consensus blocks**, ~39.9M txs @ ~16,957 tx/s. The previous crash was at ~block **395 — before** this natural end — i.e. a *spontaneous mid-run* coordinator drop (Bug 2). **This run had no spontaneous mid-run drop, so Bug 2 did not reproduce and the guard was not exercised against it** (`guard_fired=0`; the `catch`'s `error("Dropping vetted-tx…")` never logged).
- The block-436 `RpcStreamImpl: read(fd 25, n 252000212) returned 12264556 bytes` was **caused by the shutdown itself** (chuckd closing the scheduler connection mid-`submit_unvetted`), NOT the spontaneous Bug-2 trigger. The mempool handled it without a core, but this is a different (shutdown) scenario. **Do not claim the guard is on-cluster-validated** — it still rests only on unit tests + the [`publish_auth_changes`](hogd/hogdb/auth_change_subscription_manager.cc#L50-L69) precedent.
- **Exit was a crash, not graceful (see #3):** chuckd terminated via `panic: context canceled` at teardown. The C++ mempool exited without a core (no C++ crash), but chuckd itself panicked.
- Silent-failure scan was clean: 0 conservation/invariant/CHECK across mempool/hogdb/auth/event/indexer; both shards committing (shard 1 / hog2 at 5% memory-items — non-empty, so #1's regression is absent). The `433× no scp providers among known validators` is benign single-validator backfeed noise on early slots.
- **Still needed:** a rerun long enough / repeated enough to actually hit the spontaneous Bug-2 mid-run drop and confirm the mempool guard catches it (and to root-cause why the drop happens under 2 shards).

**Bug 1 fix landed 2026-07-23 (branch `vp/sharding-v2-db-abstraction`, not committed):** guarded the detached `co_await coord.coord_client_->coordscheduler_notify_vetted(response)` in [rpc_from_coord.cc:326](hogd/mempool/rpc_from_coord.cc#L326) with `try { … } catch (const x23::XdrException &e) { error(…); }`, mirroring [`AuthChangeSubscriptionManager::publish_auth_changes`](hogd/hogdb/auth_change_subscription_manager.cc#L50-L69). Also moved `pending_detached_coroutines_.fetch_sub` after the try/catch so it runs on both paths (the pre-fix code only decremented on success → a throw would also leak the counter and hang the destructor drain at [rpc_from_coord.h:88](hogd/mempool/rpc_from_coord.h#L88)). Builds clean; `Mempool.*` unit tests 34/34 pass. **Next: rerun 2-shard to confirm the transient is now survivable and to expose any deeper Bug-2 liveness stall.**
- **Found:** 2026-07-23, branch `vp/sharding-v2-db-abstraction` @ `6bd815fb`.
- **Does NOT repro on the non-sharded (single-shard) build** — per user, single-shard load runs are healthy. So the trigger is sharding-specific.

**Symptom:** 2-shard `launch-multi.sh` (hog11+hog2) comes up clean, runs healthily for ~37 min to **height ~395** (both shards taking writes — genesis fix #1 holds), then the **mempool dies** and block production freezes. chuckd exits a few seconds later as a consequence.

### Timeline (~19:45 local; hog10 mempool + hog4 chuckd logs)
1. `19:45:02` — chuckd single-validator SCP falls behind: `scpbroker.go:144 tried to repair missing scp envelopes for slot 395: no scp providers among known validators`. **Self-recovers**; slot 395 externalizes `19:45:05`.
2. `19:45:05–06` — block 395 commits; two `coordscheduler_notify_vetted` calls (xid 999/1000) succeed (chuckd side: io≈40ms, serde≈6ms). mempool's last `schedulercoord_commit_block ret` at `02:45:06` UTC.
3. `~19:45:06–08` — **mempool dies**: `RpcStreamImpl: read(fd 22, n 252000212) returned 11287156 bytes` → short read → RPC stream `fail()` → in-flight `coordscheduler_notify_vetted` resolved with an **empty `Buf{}`** → `terminate called after throwing 'x23::XdrBadInput'  what(): coordscheduler_notify_vetted: x23::Deserializer: input too short`.
4. `19:45:08` — chuckd tries to enter slot 396, needs backfeed, `no scp providers` again — no recovery (mempool gone).
5. `19:45:10` — chuckd RPC servers exit (`use of closed network connection`) — consequence of the dead mempool.

**Mempool died first; chuckd's exit is downstream.**

### Two bugs

**Bug 1 — the fatal handling (mempool robustness; PRE-EXISTING ON `main`).** The detached "launch vetted notify" coroutine at [rpc_from_coord.cc:310-331](hogd/mempool/rpc_from_coord.cc#L310-L331) does an **unguarded** `co_await coord.coord_client_->coordscheduler_notify_vetted(response)` — no try/catch. On stream failure, `RpcStreamImpl::fail()` ([rpc.cc:38-50](x23/x23/rpc.cc#L38-L50)) hands every in-flight call an empty buffer; deserializing the empty reply throws `XdrBadInput`, and since this is a `x23::Detached` coroutine with no handler, the exception is unhandled → `terminate()` kills the whole process. `git blame` → `ad9e1d32` ("mempool: add dag to submit_unvetted()"), which **is in `main`**. It never fires on `main` because the coordinator connection doesn't break there. Fix: guard the `co_await` (try/catch + check result); a dropped/degraded coordinator connection must not be fatal. **This alone would demote the crash to a recoverable blip.**

**Bug 2 — the sharding-specific trigger (data-plane; ROOT CAUSE, under investigation (b)).** The proximate trigger is the **252 MB** record length with an 11 MB short read on the coordinator connection. `default_max_message_size` is only **16 MB** ([rpc_common.cc:18](x23/x23/rpc_common.cc#L18)) yet this 252 MB length passed the size guard ([rpc.cc:82](x23/x23/rpc.cc#L82)) — so either this stream's `max_message_size_` is set very high and `notify_vetted` genuinely ships ~250 MB batches (500k+ txs), or the **stream desynced** and read a garbage length prefix from a mis-framed preceding message. Strong corroborating clue: issue #1 already recorded a **sibling** `x23::Deserializer: input too short` `XdrBadInput` on a *different* sharded RPC — auth's `mempoolauth_auth_data_changed`/`authdb_forward_key_changes` (see #1, "Separate anomaly" note). Two different sharded RPCs hitting the same "input too short" deserialize points at a **systemic serialization/framing bug in the sharded fan-out path**, not a one-off.

### (b) findings — core-dump confirmed (2026-07-23)

**Crash mechanism is 100% Bug 1, single-threaded, no shard-code fault.** `gdb thread apply all bt` on the 37 GB core: Thread 1 is the only faulting thread; its stack is exactly `reader()` → `fail()` → detached notify_vetted lambda → `trace_unhandled_exception` → `__cxa_call_terminate` → `abort`. All other ~214 threads are idle `WorkerPool` workers parked in a futex. **No concurrent `CHECK`, `process_transfers`, `scatter_gather`, or any shard-path frame** — the earlier "a shard thread CHECK-crashed first and cascaded" theory is *ruled out*. Note `fail()` invokes the pending-call callbacks **synchronously on the reader thread** ([rpc.cc:46-47](x23/x23/rpc.cc#L46-L47)), so the `XdrBadInput` unwinds *through* `fail()` — the exception isn't contained to the detached coroutine at all.

**The 252 MB is a legitimate batch, not corruption.** `ParallelBatch` stores every serialized object concatenated in one `xdr` byte-vector ([xdr_util.h:203](hogd/base/xdr_util.h#L203)); ~500k txs → ~250 MB is expected. `read(..., full=true)` ([event.h:481](x23/x23/event.h#L481)) means "suspend until the whole `len` arrives or an error occurs", so the 11 MB return = the fd was **closed/poisoned mid-transfer**, not a partial/short socket read.

**Mempool was NOT falling behind.** Its log shows a steady `nominate ret` / `commit_block ret` pair every ~6 s right up to the last block (`02:44:xx`→`02:45:06`) — zero deadline/poison/EAGAIN/backpressure/slow lines. So "sharding makes the mempool lag → connection breaks" is not supported by the mempool side.

**Conclusion on the sharding-specific trigger (still the open question):** the crash *mechanism* (Bug 1) is sharding-independent code that is **in `main`** and never fires in single-shard runs — so what differs under 2 shards is that the **coordinator↔mempool connection (fd 22) gets interrupted mid-transfer**. The mempool did not cause it (no faulting shard thread), so it originates on the chuckd side or in mempool↔chuckd backpressure. Correlated chuckd-side event: SCP single-validator stall at slot 396 (`no scp providers among known validators`) in the same 1-2 s window; the 2-shard commit path (fan-out + cross-shard reads) perturbs block timing enough to trip chuckd's envelope-backfeed/repair path, which is smooth in single-shard. Both huge RPC directions (`submit_unvetted` in, `notify_vetted` out, each ~250 MB) ride this one connection.

**The sibling clue holds:** issue #1's auth `mempoolauth_auth_data_changed: input too short` is the *same* Bug-1-class symptom (a dropped connection resolving an in-flight call with an empty buffer), not an independent serialization bug — so guarding these `co_await`s is broadly protective.

### Next steps
- (a) **Implement the Bug 1 guard first** (try/catch + result-check around the detached `co_await coordscheduler_notify_vetted`, and audit sibling unguarded coordinator/auth `co_await`s). This is the confirmed crash cause and is cheap. It converts the transient into a recoverable blip.
- Then **rerun 2-shard** — with the crash removed, observe whether block production recovers from the slot-396 SCP transient (proving it was a survivable hiccup) or truly stalls (revealing a deeper 2-shard consensus/liveness issue to chase on the chuckd/scpd side).
- If still stalling, instrument the chuckd/scpd slot-396 `no scp providers` path and the mempool↔chuckd write backpressure under ~250 MB batches.

**Repro:** `hog.conf` → `hogdb_servers=("hog11" "hog2")`; idle cluster; `./scripts/launch-multi.sh`; wait ~35–40 min / ~block 395.

**Artifacts (hog10): LOST 2026-08-06** — `/tmp` is tmpfs and hog10 rebooted, taking `mempool_exec.CRASH-2shard-notify_vetted-XdrBadInput.core` (37 GB) and `mempool_exec.CRASH-2shard-notify_vetted.log` with it. Preserve future artifacts on NFS (`/home/hog12/vinita/...`), not `/tmp/vinita`.

**Teardown gotcha:** `pkill -f 'scripts/launch-multi.sh'` self-matches the wrapper shell — use the `[x]` bracket trick (`pgrep -f 'launch[-]multi\.sh'`).

---

## 1. mempool SIGSEGV in `Scheduler::process_transfers` — 2-shard runs stall at block 0

- **Status:** FIXED + VERIFIED on-cluster 2026-07-21 (barrier + genesis dedup). Not yet committed.

**Verification (2026-07-21, 2-shard `launch-multi.sh`, hog11+hog2):** system ran healthily past genesis — no SIGSEGV / no new core on hog10, `mempool_exec` stayed alive, chuckd drove consensus to ~600 blocks in ~50s. The shard-1 smoking gun is resolved: **hog2 committed blocks (heights 589→594) with non-zero ramdb (`3% memory items`, was `0%/0B` its whole life)** and logged no `Failed to read hog age …0002`. Zero `invariant|conservation|CHECK FAILED|PANIC|mismatch` lines across all shards' fresh logs — confirming the dedup too (a double-count would trip the hog-conservation invariant on commit).
- **Found:** 2026-07-16
- **Branches:** `vp/sharding-v2-db-abstraction` @ `41636c5c`, and `vp/sharding-v2` (both rebased on main; `ctest` 2681/2681 green — bug is multi-shard end-to-end only, not covered by unit tests)

**Symptom:** With 2 DB shards, mempool crashes ~1s after startup (just past block 0) → chuckd dies with `transport is closed` → block production frozen at block 0. Single-shard runs are healthy (verified: 60+ tx batches, consensus `|D:19|X:20|`).

### Root cause (confirmed) — write/read shard-routing asymmetry: shard 1 is empty

**All DB state lands on shard 0 (hog11); shard 1 (hog2) receives nothing, yet the scheduler reads hog-ages sharded across both shards.** Proven from the preserved run (2026-07-16, hog10 core + per-host logs):

- hog2 (shard 1) hogdb: **`0% memory items`, `bytes written 0B` for its entire life; never logged `committed block 0`.** Its ramdb is empty.
- hog11 (shard 0) hogdb: committed **block 0 only**, and its on-disk `0.log` holds `…0002`'s **balance (`0x42…`) and hog-age (`0x41…`) rows**. So the loadgen accounts are seeded at **genesis** (block 0), and that genesis reached **only shard 0**.
- `shard_of_address(…0002, 2) = 1` (verified against the FNV-1a in [shard_router.h](hogd/db_shard_routing/shard_router.h)). The scheduler's `read_average_hog_ages` routes `…0002`'s read to shard 1 → hog2 (empty) → hog2 logs `rpc_from_sched.cc:75 Failed to read hog age for address …0002: Not Found`.
- Net: `…0002`'s state physically sits on shard 0, but the scheduler reads it from shard 1. Any account routing to a non-primary shard is invisible to the scheduler.

So the report's original fix #1 was directionally right but understated: it is **not just hog-ages, and not just "seeding"** — shard 1 gets no genesis/block state at all. The read path (`scheduler.cc` scatter_gather) shards by address; the write path effectively populates only shard 0.

### Why the fan-out "skips" shard 1 — it's a genesis-delta ordering race (confirmed)

`…0002` is **`kDeployContractAddress`** (`GenesisContractAddresses::kAddressInitialTokenClaim = 2`, encoded into the low bytes by [`make_precompile_address`](hogd/genesis/genesis_configuration.h#L21)) — the contract that holds the **entire initial hog token supply**, not a loadgen account. It routes to shard 1 (`shard_of_address(…0002, 2) = 1`).

The mempool genesis fan-out ([rpc_from_coord.h:216](hogd/mempool/rpc_from_coord.h#L216)) loads genesis on shards **sequentially** (`for (auto &client : db_sched_clients_) co_await client->dbsched_load_genesis_configuration(...)`), shard 0 first. Committing genesis on shard 0 is a normal block commit: [`BlockTX::commit`](hogd/hogdb/block_tx.cc#L290) → `submit_job` → `dispatch_balance_deltas` → `scheddb_hogs_transferred`, dispatched **asynchronously** by shard 0's `BackgroundCommitWorker`. Genesis is loaded **replicated in full on every shard** (each runs the whole config against its own local `ctx_.get_db()`), so shard 0's block-0 deltas include `kDeployContractAddress` (a shard-1 account). There is **no gate** in [`scheddb_hogs_transferred`/`handle_transfer`](hogd/mempool/rpc_from_db.h#L45) — the scheduler processes those deltas immediately, and `process_transfers` reads each account's hog-age routed by `shard_of_address` → reads `…0002` from shard 1 (hog2). hog2 has **not run its genesis yet** (it is the next, not-yet-executed loop iteration). Read → NotFound → `CHECK` → UB SIGSEGV → the process dies **before** the fan-out's second iteration. That is why hog2 shows zero activity: "hog2 never got genesis" is a *consequence* of the crash, not a loop bug.

(Separate, likely-unrelated anomaly: auth server hog8 logged `authdb_forward_key_changes: mempoolauth_auth_data_changed: x23::Deserializer: input too short`.)

### Crash mechanism (confirmed) — a failing `CHECK` is UB in Release builds

The binary is **`CMAKE_BUILD_TYPE=Release` → `-O3 -DNDEBUG`**. In `NDEBUG`, `x23::panic` compiles to **`std::unreachable()`** ([debug.h:194](x23/x23/debug.h#L194)), and `CHECK`/`CHECK_EQ` route through `x23::panic` ([log.h](hogd/base/log.h)). So when `process_transfers` hits `CHECK(missing.empty())` ([scheduler.cc:781](hogd/mempool/scheduler.cc#L781)) with `missing = {…0002}`, it is **undefined behavior, not a clean abort**. At `-O3` this manifests as the observed SIGSEGV during `_Unwind_Resume` in `process_transfers.actor.cold` — which is exactly why **there is no `CHECK FAILED` line in the log** and the backtrace jumps to a garbage address. (`do_panic` in a debug build would `std::abort()` cleanly; this build never calls it.)

### Recommended fixes
1. **Primary (ordering barrier) — IMPLEMENTED (2026-07-21):** genesis-block deltas must not be processed until **every** shard has finished loading genesis. Added `Scheduler::all_shards_genesis_ready_` (a `CoroFutex<bool>`, [scheduler.h:176](hogd/mempool/scheduler.h#L176)); raised after the fan-out loop in [rpc_from_coord.h](hogd/mempool/rpc_from_coord.h) `schedcoord_load_genesis_configuration` and, on restart (tip ≥ 0), in `finish_init`; waited on in [rpc_from_db.h:76](hogd/mempool/rpc_from_db.h#L76) `handle_transfer` before `process_transfers`. Builds clean; 59/59 mempool+scheduler unit tests pass. Fixes the crash; still needs the double-count fix below before cluster verification.
   - **Failure-path handling (2026-07-22, from code review):** if the genesis fan-out fails on a shard, the barrier is now released with a one-way `Scheduler::genesis_load_failed_` latch set, and `handle_transfer` drops the transfer instead of hanging forever holding `scheduler_lock_` (or reading a half-initialized shard and hitting the missing-hog-age CHECK). Recovery from a partial genesis is only a fresh restart — an in-process retry can't succeed because shard 0 already committed block 0 (`rpc_from_sched.cc` rejects re-loading genesis onto a non-empty db), so the latch is correctly one-way.

1b. **Double-count (CONFIRMED, fix pending) — companion to the barrier.** Genesis creates a real kBalance row for `kDeployContractAddress` (`FreeNativeTokenBalance` → `make_address_balance_item_key` → a `BalanceLock`, [hog_types.h:304](hogd/hogdb/hog_types.h#L304)), so genesis block 0 emits a `HogsTransferred` delta for it; `balance_lock`'s zero hog-age ([balance_lock.cc:135](hogd/hogdb/item_lock/balance_lock.cc#L135)) overwrites the direct `{supply,0,0}` write from [rpc_from_sched.cc:339](hogd/hogdb/rpc_from_sched.cc#L339), so the supply re-enters only via the scheduler adding the delta to `kHogAgeGroupReceived`. Because genesis is **replicated on every shard** and each shard's `BackgroundCommitWorker` dispatches all its deltas with no owner filter ([background_commit_worker.cc:40](hogd/hogdb/background_commit_worker.cc#L40)), the scheduler receives `kDeployContractAddress`'s block-0 delta once per shard → `kHogAgeGroupReceived = N×supply`. Block 0 has exactly this one delta (loadgen accounts are funded later via unfurl `claim_initial_tokens` txs). Fix options: **(A)** owner-shard filter at [block_tx.cc:352](hogd/hogdb/block_tx.cc#L352) — `shard_of_address(addr) == my_shard` — architecturally correct for any replicated write, but requires plumbing shard index+count into hogdb's `RPCContext` (hogdb has no shard identity today); **(B)** mempool-side genesis-scoped dedup (for `transfers.block == 0`, apply each address at most once) — contained, no protocol change, but genesis-specific.
2. **Hardening (graceful degradation):** make `process_transfers` tolerate a missing hog-age the way `apply_committed_block_hog_ages` ([scheduler.cc:202](hogd/mempool/scheduler.cc#L202)) already does (route to a skipped/`missing` set instead of `CHECK`). Stops a data-availability gap from being fatal, but masks bug (1) — do it in addition to, not instead of.
3. **Release safety (broad):** `CHECK` failures should abort with a diagnostic even under `NDEBUG`, not invoke `std::unreachable()`. Today every `CHECK` in a Release build is a latent UB SIGSEGV; this one was just the first to fire.

**Repro:** `hog.conf` → `hogdb_servers=("hog11" "hog2")`; cluster idle (`ssh hog4 pgrep -af hogjob-hogci` empty); `./scripts/launch-multi.sh`.

**Artifacts (hog10): LOST** — `mempool_exec.CRASH-dbabs-2shard.core` (this branch) and `mempool_exec.CRASH-2shard-process_transfers.core` (vp/sharding-v2) are no longer present; `/tmp` is tmpfs and did not survive the 2026-08-06 hog10 reboot. Binary `build/hogd/mempool/mempool_exec`; `gdb -q -batch -ex bt <binary> <core>`. Per-host hogdb logs under `/tmp/vinita/hogdb.log` (hog11=shard0, hog2=shard1); on-disk ramdb at `/tmp/vinita/hogdb.data/0.log`.

**Note:** `launch-multi.sh` shares hog1–hog11 with the `hogci` CI bench (mutually destructive `killall` at startup) — verify the cluster is idle first. Script traps SIGTERM and can hang on teardown; may need SIGKILL by PID + `killall -9 -e …` per host.
