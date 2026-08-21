<!-- Archived copy. The live original was /home/hog12/vinita/shard-bench/HANDOFF.md. -->

> **Archived 2026-08-21.** This is the benchmark campaign's working notebook,
> committed to the `vp/sharding-v2-archive` branch so it survives the machine it
> was written on. Three things it refers to are *not* in this repo:
>
> - **The raw run data** — ~23 GB under `/home/hog12/vinita/shard-bench/`
>   (`results/` per-run directories, `summary.json`, the report HTMLs and SVGs,
>   and the `failed-runs/`, `contaminated-runs/`, `truncated-runs/` and
>   `stalled-eventdir-runs/` sets that the traps sections refer to). On NFS, so
>   it survives reboots; too large to commit.
> - **Core dumps** — every path under `/tmp/vinita/` on a `hog*` host is tmpfs
>   and is gone after that host reboots. Several are already lost, and the entry
>   that lost them says so.
> - **The campaign harness** — `shard-bench/bin/*.sh`, the scripts each "Repro"
>   line names.
>
> Read [BRANCHES.md](BRANCHES.md) first if you want the code rather than the
> measurements, and [ISSUES.md](ISSUES.md) for the defect log. Where this file
> and ISSUES.md disagree, ISSUES.md is newer: it was pruned and re-checked on
> 2026-08-21, and this file is left exactly as the campaign ended.

# hogd sharding benchmark — handoff

Branch `vp/sharding-v2-db-abstraction`. Everything below was measured on the hog
cluster between 2026-08-10 and 2026-08-15. Report:
https://claude.ai/code/artifact/f65edc89-65d2-4ed6-8869-666c91f837cb

## The one thing to know first

**The mempool holds at most 5,000,000 transactions and silently discards the
rest.** `kDefaultMempoolTxSizeLimit` in `hogd/mempool/rpc_from_coord.h:66`,
hardcoded, with a `TODO(jhumphri)` asking for it to be a command-line arg. In
`submit_unvetted` (`rpc_from_coord.cc:216-280`) a full mempool rejects the whole
batch; a partly-full one truncates and bins the remainder.

This caps every capacity result. Offering 1.3 billion transactions committed
only 78.8M (6%); 3.25x the offered load bought 1.25x the commits. At ~369 bytes
of state per committed transaction, live state cannot exceed ~30 GiB per run
regardless of cache size, shard count or duration. **Raising this constant is
the prerequisite for any large-capacity demonstration.**

## Answers to the original questions

### Is ISSUES.md #6 fixed?

Yes, the trigger is. Root cause was hogdb filling a 189 GB tmpfs (141 GB
written) -> ENOSPC -> `DbSchedResultError` -> a `CHECK` that is UB under NDEBUG.
Fixed operationally by pointing `--db-path` at real disk
(`scripts/launch-multi.sh`, `hogdb_data_dir=/var/tmp/$(whoami)/hogdb.data`).
Multiple 1-shard runs at the exact repro config (`--txs-per-block 100000`) now
pass through work batch 728-758, where all three original crashes landed, having
written the same ~139 GB, with zero I/O warnings and no new cores.

**The mechanism is untouched.** `commit_block` still `CHECK`s the db result.
Three *different* causes have now produced that same fatal `DbSchedResultError`:
ENOSPC (#6), the per-shard invariant (#7), and a too-small `--ram-bytes`. Any
future db error will still kill the mempool with no diagnostic under NDEBUG.
ISSUES.md #6 rec 1 remains open and is the highest-value fix in this area.

### Does sharding improve performance?

No, on either axis. Two executors, 1M-tx batches, matched block windows:

| shards | tx/s | vs 1 shard | % of linear | median block |
|---|---|---|---|---|
| 1 | 211,182 | — | — | 4.11 s |
| 2 | 214,325 | +1.5% | 51% | 4.17 s |
| 4 | 196,985 | −6.7% | 23% | 4.59 s |

Throughput is flat then declining; typical block latency worsens ~12% by four
shards while p90 *improves* (9.03 -> 5.75 s). Splitting a block into phases:
**commit gets 24% faster with shards (2,194 -> 1,666 ms) while nomination gets
61% slower (1,352 -> 2,180 ms)** and swamps the gain. Nomination is where the
scheduler does its per-address hog-age reads; `scatter_gather`
(`hogd/db_shard_routing/sharded_client.h:46`) batches correctly (one RPC per
shard, not per account) but waits for the slowest shard, so cost grows with
shard count. **That is the specific engineering target if sharding throughput
matters.**

Executors do matter, unlike shards: 1 executor vs 2 is −1.5% at one shard but
−21% at four. Sharding creates executor work.

### Does sharding improve capacity?

Yes, and this is the only place it wins. With `--num-virtual-accts` making state
grow with the transaction count, at 10 GB per shard:

| shards | occupancy | outcome |
|---|---|---|
| 1 | 78% | **stalled — mempool aborted** after 33M txs |
| 2 | 77% / 35% | completed, 40.5M txs |
| 4 | 62% / 20% / 21% / 20% | completed, 39M txs |

Both unsharded runs failed; every sharded run finished. Note the occupancy is
**deliberately imbalanced** — shard 0 carries 2-3x its peers (confirmed
intentional by the user, do not investigate).

## Bugs found and written up

- **ISSUES.md #7** — hogdb's hog-conservation invariant is whole-db but
  evaluated per shard, so any assertion-enabled multi-shard run dies at genesis
  with equal-and-opposite `total_supply_change_` on each shard. Only bites
  builds with assertions live.
- **ISSUES.md #8** — the event server's `--work-dir` is never deleted between
  runs; it reached 146 GB on hog9's tmpfs and stalled five consecutive runs.
  Fixed in `launch-multi.sh`. Also records that chuckd's
  `panic: e.g. memory allocation failure` is **not** an OOM — it is chuckd
  printing the XDR *comment* for `SYSTEM_ERR = 5` (`x23/x23/rpc_msg.x:38`).
- **Release builds were not NDEBUG.** `external/crc32c/third_party/benchmark/
  test/CMakeLists.txt:13` does `add_definitions(-UNDEBUG)` for every non-Debug
  build, landing after `CMAKE_CXX_FLAGS_RELEASE`. Worked around with one
  `add_compile_options($<$<NOT:$<CONFIG:Debug>>:-DNDEBUG>)` after
  `add_subdirectory(external/crc32c)` in the top-level `CMakeLists.txt`
  (uncommitted). Needs a proper decision.
- **`--ram-bytes` below ~1 GB** makes hogdb fail genesis outright with
  `to_db_result error: Internal Error`, and crash rather than degrade.

## Uncommitted changes to tracked files

- `CMakeLists.txt` — the NDEBUG re-assert above.
- `scripts/launch-multi.sh` — `hogdb_data_dir` (off tmpfs), deletes the event
  work dir, builds only the 7 needed targets (building everything hits x23's
  `bench`, which does not compile against the vendored benchmark headers), and
  `ram_bytes` / `num_accts` / `num_virtual_accts` / `txs_per_block` /
  `num_blocks` made configurable.
- `ISSUES.md` — issues #7 and #8.
- `hogd/loadgen/*` and `virtual_address.h` — `--num-virtual-accts`, added by the
  user, not by the agent.

## Harness (`/home/hog12/vinita/shard-bench/`)

`bin/run-one.sh` is the core: one run, any shard/executor/cache/workload
combination via env vars (`TXS_PER_BLOCK NUM_BLOCKS RAM_BYTES NUM_ACCTS
NUM_VIRTUAL_ACCTS EXEC_HOSTS_OVERRIDE START_TIMEOUT MAX_MINUTES`), collects
everything into `results/<tag>/`, writes `verdict.txt`. `bin/analyze.py` ->
`summary.json`; `bin/final-report.py` -> `sharding-report.html`.
`bin/cleanup-cluster.sh` wipes all 11 hosts. `bin/hogci-busy.sh` gates on
aggregate load only — **it deliberately does not inspect other users'
processes**; the user asked for that explicitly.

Sweep drivers: `crossover.sh`, `bigmem.sh`, `execscan.sh`, `capacity-probe.sh`.
Each has a watchdog sibling. Results are named
`<shards>shard-<variant>-<seq>`; `analyze.py` finds runs by the presence of
`verdict.txt`, not by name.

## Traps that cost time — do not repeat

1. **Never edit a script while it is running.** Bash reads incrementally; three
   runs were lost this way, including a collector that then silently wrote
   nothing for four runs.
2. **`pgrep -f <pattern>` matches your own shell.** Use `pgrep -af 'foo[b]ar'`.
3. **Compare like with like.** Throughput and block latency both *decay within a
   run*, so whole-run averages measure how long a run survived. Every comparison
   here uses a matched block window. Two conclusions were briefly wrong from
   this: "2 shards 19% faster on latency" (it is ~15% slower) and a "30% faster"
   small-batch figure.
4. **A run's `COMPLETE` does not mean the workload finished.** chuckd stops
   submitting long before commits drain, and at high load it dies via
   `panic: context canceled` (ISSUES.md #3). The drain loop now checks chuckd is
   alive before treating a plateau as drained.
5. **The cluster is shared and hostile.** Other jobs killed or contaminated ~8
   runs; ssh to hog10 fails under their load, which reads as `NO_START`. Every
   run records per-host co-tenant CPU; contamination is judged from that, not
   from process names. hog11 has ~100 GB of another user's tmpfs resident, so it
   is last in the shard host order.
6. **Verify a flag reached the binary.** `--num-virtual-accts` existed in source
   while the built `loadgen_server` was three days stale and ignored it.

## Suggested next steps, highest value first

1. **Make `mempool_tx_size_limit` a command-line arg** and re-run the capacity
   ladder with ~50M. Everything about large-capacity demonstration is blocked on
   this one constant. The TODO is already in the code.
2. **Fix `commit_block`'s `CHECK` on the db result** (ISSUES.md #1 rec 3). Three
   unrelated causes have now killed the mempool through it, each time with no
   usable diagnostic.
3. **Nomination's `scatter_gather` cost** — the 61% growth is the whole reason
   sharding shows no throughput gain.
4. Re-take the 4-shard throughput runs; their per-host CPU traces are missing
   (collector was broken by trap 1), so they are one notch less verified.

---

# Session 2 — 2026-08-17: the mempool tx limit, and a preload that fills a cache

## Read this first: a counter in the old analysis is wrong

**`mempool_commit_removed_tx_count` is not a count of committed transactions.**
It equals (blocks committed x `kMaxBlockSize`) + 20,002 genesis in every run
measured — it charges a full 1,000,000 per commit whatever the block actually
held, and blocks are usually far from full. On `1shard-75gb-01` it reads 269M
against chuckd's 78.8M.

Use the sum of `Num txs in block` from chuckd's own `block-stats.txt`, which is
what `bin/bigcache-report.sh` now does. It is corroborated by state size:
30.9 GB / 78.8M = 392 B/tx, matching the independent 369 B/tx calibration. Any
figure derived from the mempool counter is inflated ~3x.

## What raising the mempool tx limit bought

`kDefaultMempoolTxSizeLimit` 5,000,000 -> 50,000,000 in
`hogd/mempool/rpc_from_coord.h:66` (uncommitted). Verified in the binary: one
`$0x2faf080` immediate, no `$0x4c4b40`.

| | offered | admitted | rejected RPCs | discarded | committed |
|---|---|---|---|---|---|
| `vacct-5m-verify` (5M) | 20M | 12.0M | 7 | 1,000,000 | 5,000,000 |
| `vacct-50m-01` (50M) | 20M | **19.0M** | **0** | **0** | **8,000,000** |
| `2shard-xover-01` (5M) | 200M | 71.0M | — | — | 40,000,000 |
| `2shard-xover50-01` (50M) | 200M | **125.0M** | — | — | 42,056,232 |

**The limit bounds the pending backlog, not a run's lifetime total** — it gates
`scheduler_->size()` in `schedulercoord_submit_unvetted`
(`rpc_from_coord.cc:216-281`). So on a 20-second burst, where the backlog *was*
what limited how much work existed to commit, raising it gave **+60% commits**.
On a 200-batch run, admitting 76% more work gave **+5% commits**: commits are
drain-rate-bound, and the surplus just becomes backlog. Cost: ~0.86 KB of
mempool RSS per pending tx, so 41-54 GB at a full 50M, and a 50M-entry scheduler
is a credible reason nomination slows (74.5k vs 115.8k tx/s on the 2-shard pair,
but that run had 6 interference warnings — needs a clean repeat).

**So the limit was never what blocked the big-cache runs.** `bigmem.sh`'s header
blames load shedding for abandoning 250 GB; shedding was a symptom. The wall is
commit throughput (~20k tx/s at large state), which is why 300 GB needs ~810M
commits and ~10 hours per arm. `1shard-300gb-01` is the demonstration: 55 min of
full load reached **3% of 279.4 GiB** (and interference 66, so it is a floor).

## The preload hack — how to fill a large cache in minutes

`hogdb --preload-items N --preload-bytes-per-item S` (`hogd/hogdb/hogdb.cc`,
`preload_synthetic_items`, BENCHMARK HACK, not for merge), plumbed through
`launch-multi.sh` (`preload_items` / `preload_bytes_per_item`) and the harness
(`PRELOAD_ITEMS` / `PRELOAD_BYTES_PER_ITEM`, recorded in `verdict.txt`).

It writes storage items (`write_nnint128` / `write_bytes`) at
`make_virtual_address(i)` — the loadgen's own derivation, so the filler
distributes across shards exactly as real load does — skipping any address this
shard does not own, so the shards' partial db hashes still sum. It runs *before*
genesis, which recomputes its base db hash over pre-existing direct writes, so
the hash accounts for it. Storage items carry no hogs, so the hog-conservation
invariant cannot fire.

Measured: **4,160 bytes per item** at `--preload-bytes-per-item 4096`; **2.3 GB/s**
locally, **0.85 GB/s** on hog1 through the disk-backed data dir. So ~6 minutes
per 300 GB shard. Small items (`nnint128`, no `-bytes-per-item`) are ~80 B each
at 1.7M items/s — too many items for a large cache, use the 4 KiB mode.
**65,000,000 items ~= 270 GB, i.e. 90% of a 300 GB shard.** `preload_items` is a
whole-db count: multiply by the shard count.

Validated end to end (`preload-validate`): 3.6M items in 17.3 s, occupancy 76%
of 18.63 GiB, genesis loaded over it, and the workload committed 8,000,000 —
identical to the same workload on an empty cache.

**Limitation, and it matters for what can be claimed.** The filler is *cold*.
The workload pays ERC20 balances, which are keyed under the single
`erc20_address_` contract (so the workload's own state concentrates on one shard
by construction), while the filler is storage items at virtual addresses. An LRU
cache evicts filler before hot state, so this measures **capacity and throughput
with a full cache** (300 GB / 600 GB / 1.2 TB resident), NOT a working set
spilling to disk on one shard but not on four. Making the filler hot means
preloading balances, which invents hogs and risks the missing-hog-age crash path
of ISSUES.md #1.

## New issues written up

- **ISSUES.md #9** — every benchmark abandons its backlog. The driver loop logs
  `waiting for work batch N to commit` and then does not wait for a commit; it
  blocks on a batch the loadgen will never produce
  (`coordinator_loadgen/system.go:258-277`). Block production stops there, so
  runs end with 5M-10M admitted transactions never committed. Every
  `N of M committed` figure is short by whatever was still pending, and a commit
  plateau does not mean "drained".
- **ISSUES.md #10** — at a 50M backlog (41.8 GB RSS) hogdb died silently on hog1
  with a 15.7 GB core, healthy one second earlier, no OOM; the mempool then
  terminated on the dropped connection via an unguarded `co_await` in the
  *detached* `MempoolDbRpc::handle_transfer` — **ISSUES.md #2's defect at a third
  call site**. hogdb's root cause is still open; its core is preserved on hog1.

## Traps added to the list

7. **Any `git checkout` in hogd resets the submodules.** A `post-checkout` hook
   runs `git submodule update --init --recursive`, which snaps `protocol` and
   `groundhog-go` off the interim `vp/shard-card-identity` branches; the tree
   then fails to build with `'ShardFunction' does not name a type`. Even
   `git checkout -- one_file.cc` fires it. Undo a file edit by editing it back.
8. **Never start a driver while the previous one is exiting.** Each sweep's EXIT
   trap runs `cleanup-cluster.sh` across all 11 hosts, and that script deletes
   `$LOGDIR/*.log` and the hogdb data dir **without killing any processes** — so
   it strips a live run's database out from under it. That is what killed
   `1shard-xover50-01` (quarantined in `contaminated-runs/` with a note): its
   `db_bytes` went 403 MB -> 0 and every collected log came back empty. Wait for
   the previous `run-one.sh` to exit, or `kill -9` the driver so its trap never
   runs.
9. **The `hogci` CI fleet was on the cluster most of this afternoon** (clang++ /
   cc1plus, load 50-64 on eight hosts). `hogci-busy.sh` gates the start but
   nothing gates mid-run, so today's throughput figures are floors. Re-take
   anything headed for a talk on a quiet cluster.

## Harness additions

`progress.csv` gained `mempool_rss_kb` (a 50M backlog makes hog10's memory a
plausible way to lose a run); `verdict.txt` gained `mempool_tx_size_limit`,
`preload_items` and `preload_bytes_per_item`; `analyze.py` reads all of them.
New drivers: `txlimit.sh` + `txlimit-report.sh` (the limit A/B), `xover50.sh` +
`xover50-report.sh` (crossover at the new limit), `bigcache.sh` (300 GB by
traffic alone), `bigpreload.sh` + `bigcache-report.sh` (300 GB via preload).

---

# Session 3 — 2026-08-18: the limit raise was a mistake, and what still blocks big state

Report (plots + all findings): https://claude.ai/code/artifact/3f8765dc-4948-487e-8742-9f8da72e8daf
Regenerate it with `bin/report2.py`; republishing the same path keeps that URL.

## Reverse session 2's recommendation: put the mempool limit back to 5M

Session 2 measured `kDefaultMempoolTxSizeLimit` 5M -> 50M on a 20-second burst,
where the backlog genuinely was the constraint, and concluded it helped. On real
runs it does not. Five matched pairs, same workload, same hosts, limit the only
difference:

| ladder | shards | committed | steady tx/s |
|---|---|---|---|
| 10 GB | 2 | 40.0M -> 42.1M (+5%) | 115,784 -> 74,527 (**-36%**) |
| 10 GB | 4 | 40.0M -> 41.3M (+3%) | 110,691 -> 70,525 (**-36%**) |
| 75 GB | 1 | 78.8M -> **69.4M (-12%)** | 7,923 -> 6,999 |
| 75 GB | 2 | 78.8M -> **68.0M (-14%)** | 8,481 -> 7,940 |
| 75 GB | 4 | 78.8M -> **67.1M (-15%)** | 7,413 -> 7,847 |

At 10 GB it trades a third of throughput for a few percent more commits; at
75 GB it loses on both axes. Two shard counts landing on -36% independently is
what rules out cluster contention, which was the first reading when only the
2-shard arm showed it. Mechanism: the limit bounds the *pending backlog*, so on a
long run the surplus becomes backlog rather than commits, and a scheduler holding
tens of millions of entries is slower to nominate. Cost side: 41-56 GB of mempool
RSS, ~0.86 KB per pending tx.

**Also: three of the arms that died mid-run were at 50M with tens of GB of
backlog, while every 5M arm completed.** Not conclusive (one 50M arm finished),
but it is the pattern to chase -- see the hogdb deaths below.

## Sharding still buys no throughput

At the old limit 1/2/4 shards committed 78.8M / 78.8M / 78.8M; at the new limit
69.4M / 68.0M / 67.1M -- flat, declining slightly with shard count. State in every
one of these arms is 8-31 GB, comfortably inside a single shard's cache, so this
measures sharding's coordination cost with none of its capacity benefit. Nothing
here contradicts session 1; it extends it to a second cache size and a second
mempool limit.

## Why no run has yet produced a capacity crossover

The demonstration that matters -- one shard spilling to disk while four do not --
needs state larger than one shard's cache. Every route was tried:

1. **Transaction traffic.** State grows ~392 B per committed transaction, so
   300 GB needs ~765M commits: 10 h/arm at the ~20k tx/s this system sustains at
   large state. The 2600-batch arms now running are the practical limit of this
   route (~60 GB).
2. **Injecting state into the ramdb** (`hogdb --preload-items`, see below). Fills
   269 GB in 282 s and works at **one shard only**: each shard's pre-genesis
   content differs, so their genesis hashes differ and the mempool refuses to
   start with `db shard tip divergence`. Only `kHogAge` and `kGlobalMetadata`
   escape the consensus hash (`is_state_item`, hogdb_dao.h) and hog-age rows are
   fixed ~100-byte records -- 700M of them to fill 141 GB, back inside the
   per-item cost collapse. Replicating identical items on every shard would keep
   the hashes equal but break the invariant that each item is hashed on exactly
   one shard, describing a database that cannot exist. Do not revive that idea.
3. **Genesis accounts.** 500k wallets kills chuckd: the loadgen needs minutes to
   build them, `endpoint_call_timeout_msec` is 2000, and chuckd panics with
   `unexpected EOF`. Per-account state is 4-30 KB anyway, so even 500k wallets
   adds less than transaction traffic already does. Abandoned.
4. **Contract-generated state** -- the remaining route, and the only one that both
   scales and partitions correctly. Half-built, see below.

## The preload, for the record (1 shard only)

`hogdb --preload-items N --preload-bytes-per-item S` writes synthetic storage
items straight into the ramdb before genesis. Results:

| arm | cache | occupancy | live state | committed | tx/s |
|---|---|---|---|---|---|
| `1shard-preload-01` | 200 GB | 76% of 186.3GiB | 140.7 GB | 52,743,553 | 38,789 |
| `1shard-300gbcache-01` | 300 GB | 93% of 279.4GiB | 260.2 GB | 52,147,640 | 38,546 |

**Throughput is flat within 0.6% while live state nearly doubles** -- at one
shard, state size in this range is not what limits throughput.

Two sizing facts, both measured:

- **Write cost grows with item count.** 4 KiB items ran at 2.3 GB/s into an empty
  db and 64 MiB/s by 33M items -- 35x decay, not disk (idle) and not memory.
  32 KiB items need 8x fewer inserts for the same bytes and hold ~1 GB/s. 64 KiB
  is rejected outright (`Too Big`). The same write path serves genesis.
- **A filled 300 GB shard fits only hog1.** hog1 has 755 GB; the other ten hosts
  have 377 GB. The ramdb needs headroom above `--ram-bytes` for compaction and
  disk buffering: hog2's hogdb died with `still out of memory; compacting all
  loglets` partway through a 300 GB fill that hog1 completed, and hog1 filled to
  90% then compacted continuously. A uniform ladder tops out at 200 GB/shard
  filled to ~70%.

## hogdb dies silently mid-run, and the harness cannot see it

**Three arms lost to this.** hogdb's last log line is a *successful*
`committed block`, then it is gone, leaving a multi-GB core, with no error and
ample free memory. The mempool survives, and `run-one.sh` decides liveness from
`pgrep mempool_exec` alone, so it kept polling a dead run until the 10-minute
stall timer (or, before the first batch, the 60-minute start timeout) expired.

- Same signature as ISSUES.md #10, which now has three occurrences.
- `bin/health-watch3.sh` closes the gap: it checks hogdb on every shard host plus
  chuckd, and kills an arm whose shard has been missing ~6 min so the sweep
  advances. Run it as a monitor alongside any sweep.
- Cores kept: `hogdb.1449394.core` (hog1, 01:20). hogdb has no resolvable
  symbols, so `gdb ... bt` yields only `?? ()` frames -- the log line is the
  evidence, not the core. `evidence/hogdb-deaths/` has the log tails.

## The CI shares these hosts and shares their ports

Five arms died to `Address already in use` before this was understood: hogci runs
the same hogd stack, so it binds the same ports. Fixed by moving ours:

- all 24 cluster ports in `scripts/launch-multi.sh` -> 85xx range;
- chuckd's own ports -> in-addr 7988, metrics 2323, debug 5600, out 50100, and
  its scpd control/handle -> 127.0.0.1:4450 / :3350;
- `run-one.sh`'s metrics scrapes follow (8575 mempool, 8583 hogdb).

Note `wait-port.sh` cannot tell our service from theirs -- a foreign listener
satisfies the check, which is how one run "passed" the auth port wait while our
auth server had already died on the bind. Every `killall` in the harness is now
`-u vinita`; before that it was trying (and failing, with `Operation not
permitted`) to kill hogci's processes.

Also: **chuckd cannot survive a long wait.** It registers with the mempool, the
mempool is ready only once every shard has registered, and with a preloading
shard that is minutes later -- long enough for chuckd's idle RPC transport to
scpd to die, after which `SCPSetKnownQuorumSet` panics through `must.NoError`.
`launch-multi.sh` now starts chuckd *after* the db port waits.

## Traps added

10. **`pgrep -f` matches the shell that wrote the script.** `overnight4.sh`
    guarded with `pgrep -f 'run-one\.sh'`; the parent shell's command line
    contained the entire heredoc, including that literal string, so the sweep
    waited on itself for 7h48m and the cluster sat idle from 05:45 to 09:16. The
    bracket trick does not help -- the parent's argv contains the real text. Guard
    on something a heredoc cannot contain (`'run-one\.sh [0-9]'`) and prefer
    `setsid` so the writer is not an ancestor.
11. **`mempool_commit_removed_tx_count` is not a transaction count.** It equals
    blocks committed x `kMaxBlockSize` + genesis in every run measured -- 269M
    against chuckd's 78.8M on `1shard-75gb-01`, a 3.4x overstatement. Use the sum
    of `Num txs in block` from chuckd's `block-stats.txt`; state size corroborates
    it at ~392 B/tx. `bin/bigcache-report.sh` and `bin/report2.py` do.

## storage_filler: built, not yet wired to the loadgen

The remaining route to large *partitioned* state. Written and building:

- `built_contracts/integration_test_contracts/storage_filler/` -- 65 lines,
  entrypoint `fill(nonce: u64, count: u32, item_bytes: u32)` doing `count`
  `bytestring_set` writes of up to 4 KiB each under keys derived from
  (nonce, index).
- Registered in `built_contracts/Cargo.toml`, and in `CMakeLists.txt` three times
  (`add_wasm_builds`, `add_riscv_builds`, `add_contract_header`). Both the wasm
  and rv32 builds pass; `build/built_contracts/storage_filler.h` exposes it to
  C++.
- **It lives in the `integration_test_contracts` submodule.** Commit it there
  before any `git checkout` in hogd, or the post-checkout hook's
  `submodule update` deletes it (see [[hogd-checkout-resets-submodules]]). A copy
  is in the session scratchpad.

Remaining work, all in `hogd/loadgen/erc20_experiment.cc`:

1. Deploy N instances at genesis via `builder.add_initial_config(filler_contract,
   make_nonce(i), entrypoint<"fill">(), make_calldata<"fill">(0,0,0), hogs)` --
   `fill` with count 0 is a no-op, so no initializer is needed. **N must be large
   (~256): storage items are keyed by the *contract's* address, so one instance
   pins all its state to one shard** -- which is also why the workload's own ERC20
   balances concentrate today.
2. In the work-tx path, build the inner calldata against a rotating instance:
   `x23::serialize(filler_address_[i], entrypoint_fill, GasAmount{...},
   make_calldata<"fill">(nonce, count, item_bytes))`, wrapped in the wallet's
   `do_action` exactly as the erc20 transfer is.
3. Flags for instances / items-per-tx / bytes-per-item, plumbed like
   `--num-virtual-accts`.
4. Storage cost is *not* a constraint: 12,000 + 10/byte against a 6.25e17 hog
   supply, so 400 GB as 1 KiB items costs 0.0014% of supply. The open question is
   the per-transaction write limit against `work_gas_limit()` -- settle it on
   hog-devnode, not on the cluster.

Fill-rate arithmetic: at 8 KB of state per tx and ~10k tx/s that is 80 MB/s, so
~400 GB over a normal 400-batch run -- enough to overflow one shard and fit four,
which is the crossover no other route reaches.

## Harness additions this session

`bin/report2.py` (the report + plots; palette validated for light and dark),
`bin/health-watch3.sh` (per-shard liveness with auto-recovery),
`bin/bigcache-report.sh` (the trustworthy committed count),
`bin/overnight3.sh` / `overnight6.sh` (sweeps ordered by what is *missing*, not by
shard count -- this campaign accumulated six 1-shard arms before its first
4-shard one), `bin/txlimit-report.sh`, `bin/xover50-report.sh`,
`evidence/hog2-oom-cores/`, `evidence/hogdb-deaths/`.

## Postscript — the crossover ladder, and the one configuration where sharding wins

Run after the ladders above, once the storage_filler contract was wired into the
experiment chuckd actually requests. Same workload, same 75 GB cache per shard,
every 4th transaction writing 16 KiB of contract storage:

| shards | committed | steady tx/s | disk (total) | disk per host | resident |
|---|---|---|---|---|---|
| 1 | 33,364,549 | 51,054 | 105 GB | 105 GB | ~10.5 GiB (15%) |
| 2 | 37,975,126 | **65,242 (+28%)** | 123 GB | 65 / 58 GB | ~10.5 GiB (10+5%) |
| 4 | 39,710,910 | **73,880 (+45%)** | 129 GB | ~32 GB each | ~10.5 GiB (7+3+3+2%) |

**Read the "state" column in the harness as DISK, not memory or live state.**
`run-one.sh` measures `du --apparent-size` of the hogdb data dir summed over shard
hosts. hogdb is a cache over disk, so 105 GB on disk against a 69.85 GiB cache is
not an overflow -- and a log-structured store also holds superseded and
uncompacted bytes, so the figure is an upper bound on live state. Earlier drafts
of the report called this "live state"; that was wrong.

**Throughput rises monotonically with shard count -- the only ladder in this
campaign that does -- but it is write-path parallelism, not a memory-capacity
win.** Resident memory was ~10.5 GiB in *all three* arms: total occupancy stays at
15% of one cache's worth however many shards are added, because the filler's
writes are never read back and the ramdb passes them through to disk. The extra
225 GB of cache the 4-shard arm has is never demanded. What actually scales is the
write path -- more disk writers, more shards' worth of db work and locks -- and
the per-host disk footprint falling 105 -> 62 -> 32 GB is the distribution
working. The reason is what a transaction costs. The transfer
workloads write ~392 B of state each, the database is never the constraint, and
shards only add coordination: flat-to-declining throughput. A filler transaction
writes **15,284 B (measured by the calibration arm)**, the database becomes the
bottleneck, and splitting it is exactly what helps.

Two caveats that belong with the number:

1. **The state is cold.** `fill_key` derives from the transaction's own nonce, so
   every write is a fresh insert and nothing reads it back. The 1-shard arm holds
   104 GB against a 69.85 GiB cache and never pays to re-read what spilled -- this
   is a write-path result, not a read-penalty one. The sharper experiment is a
   bounded keyspace (`j % K` instead of a unique nonce) so writes become
   read-modify-writes over a hot working set; that is the run that would show
   sharding winning on read latency.
2. **The arms match on offered load and duration, not on final state.** More
   shards commit more and therefore accumulate more state. That follows from the
   throughput result rather than confounding it, but do not describe the arms as
   holding equal state.

### The bug that cost the first attempt

chuckd requests `KExperimentTypeERC20BasicAuthPayments`
(`groundhog-go/chuckd/sub/loadgen/loadgen.go:85`), so the cluster runs
`ERC20BasicAuthExperimentState` -- **not** `ERC20ExperimentState`. The filler was
wired into the latter first, so every "filler" transaction was an ordinary
transfer: the flags reached the binary, the contract deployed, nothing errored,
and the only symptom was the calibration number coming back at 182 B/tx instead
of ~15,000. The devnode gas measurements were real but exercised a path the
cluster never executes. **Both experiment classes now carry the knobs; change
them together.**

Sizing, for anyone repeating this: at 15,284 B per committed transaction and
`--filler-period 4`, 200 batches puts ~104 GB on one shard and ~129 GB across
four. `--filler-period 1` would attempt ~650 GB against 379 GB of disk, which is
ISSUES.md #6's ENOSPC crash rather than an experiment -- the ladder driver checks
free space before each arm for that reason.

## Default-workload performance matrix (2026-08-18, mempool limit back at 5M)

Default workload -- 100 batches x 1M txs, 10 GB cache per shard, 10k accounts, no
virtual recipients, no filler, no preload -- across executor and shard counts.
Steady-state tx/s over a matched block window; every cell below had <= 4 CI
interference warnings.

| | 1 executor | 2 executors | 3 executors |
|---|---|---|---|
| 1 shard | 178,588 | 197,986 | 195,945 |
| 2 shards | 181,369 | 200,768 | 205,819 |
| 3 shards | 173,831 | 193,405 | **212,517** |
| 4 shards | 156,458 | 201,857 | — (cluster is a host short) |

1. **Read the rows: the executor count at which throughput saturates rises with
   shard count.** One shard is finished at two executors (197,986 -> 195,945, no
   gain from a third). Three shards is still climbing at three (173,831 ->
   193,405 -> 212,517, +11% then +10%). Sharding fans execution work outward, so
   a sharded configuration needs more executors to reach the same ceiling.

   This corrects an earlier reading in this file's drafts. Looking only at the
   1-shard row says "two executors is enough, the ~200k ceiling is not the
   executors" -- true for one shard, false for three. Do not generalise a single
   row.
2. **Sharding costs when execution is the constraint.** The single-executor column
   is the only place shard count matters and it goes the wrong way: 178k -> 181k
   -> 156k, a 22% penalty at 4 shards, because sharding fans execution work out.
3. **4 shards x 3 executors is missing for a host-count reason, not an
   oversight**: at four shards the databases occupy hog1/hog2/hog3/hog11 and every
   remaining host runs a service (chuckd, loadgen, auth, event, mempool), so a
   third executor would have to double up and confound the comparison.
4. **3 shards works.** `shard_of_address` is `h % shard_count`, a true modulo, so a
   non-power-of-two count distributes evenly (9%/9%/9% occupancy measured).
   `run-one.sh` now has a 3-shard case using hog1/hog2/hog3, leaving hog11 free as
   a third executor. Its 212,517 is the campaign's best figure but is one arm,
   inside the spread cluster noise produces -- not evidence that odd counts are
   faster.

**Treat any single arm with `hogci_interference` above ~5 as unusable.** The
2-shard/3-executor arm first came back at 149,925 tx/s with 22 warnings; re-run on
a quiet cluster it was 205,819 with zero. The busy gate only checks *before* an arm
starts, so a run can begin clean and be swamped mid-flight. `bin/redo-2s3e.sh` is
the pattern for this: retry until an arm lands with interference <= 5.

**Port collisions with the CI, final tally.** Six separate ones today, each fatal
because every service aborts on a bind failure rather than retrying, and
`wait-port.sh` cannot tell our service from theirs on the same number (a foreign
listener satisfied the auth-port check while our auth server had already died on
the bind). Every range is now private: cluster services 85xx, executors 8640/8641,
chuckd 7988/2323/5600/50100, scpd 127.0.0.1:4450+3350, loadgen 11322/11323. The
real fix is bind-with-retry or a per-user port base; worth an ISSUES entry.

**Housekeeping now automatic.** `bin/reap-cores.sh` deletes cores that no write-up
names and that are over an hour old -- cores are 1-38 GB on tmpfs, and 37 of them
had pushed hosts toward run-one.sh's 70% abort threshold (that is what aborted
2shard-small-01). Run it from a loop during a sweep. Inventories of what it removed
are in `evidence/cores-deleted/`.

## multiexec_runner, now multi-shard (2026-08-20)

`hogd/hogexec/bench/multiexec_runner.cc` used to start exactly one hogdb and
drive it through one `db_sched_client`. It now takes `-d <num_db_shards>`: it
starts that many hogdbs with `--shard-id`/`--total-shards`, waits for that many
shard cards, registers all of them with the executors, and fans the genesis load
and the three snapshot calls (`create`, `close`, `commit`) across every shard --
the same lifecycle `ShardedDbSched` runs in the mempool. Host budget went from a
hardcoded 3 to `num_db_shards + 2`.

This is the only sharding measurement in the campaign taken without consensus in
the loop: a mock scheduler opens a block, hands one million transfers to the
executors and closes it. Nothing waits on nomination or on a quorum, so whatever
sharding does here it does to the execution and database path alone.

Results, `-a 100`, batch_size 10, five one-million-transaction trials per cell,
executors pinned to hog3/hog5/hog6 across every configuration
(`bin/mx-sweep.sh`, `bin/mx-summary.py`, `results/multiexec/`):

| shards | 1 exec | 2 exec | 3 exec |
|---|---|---|---|
| 1 | 475,227 | 717,314 | 1,030,109 |
| 2 | 358,665 | 628,632 | 836,968 |
| 4 | 241,973 | 441,438 | 636,048 |

Two things to take from it:

  1. Executor scaling is near-linear inside every shard count (three executors
     are worth about three times one), and the shard lines never cross. Splitting
     the database costs throughput on this path at every executor count -- ~19%
     at two shards and ~38% at four, at three executors -- and the cost grows
     with the split. It buys capacity, not speed.
  2. The full stack tops out near 200k tx/s while this path reaches 1.03M, which
     is the same conclusion the full-stack ladder reached from the other side:
     nomination and commit are the ceiling, not execution.

The one bias to keep in mind: this bench issues the snapshot calls to the shards
sequentially where the mempool overlaps them, so its per-block overhead grows
with shard count in a way production's would not. Sized rather than assumed --
close+commit goes 48 ms (1 shard) -> 55 ms (2) -> 78 ms (4) per block, which is
2-5% of the measured slowdown. The remaining 95%+ is executor-side work: every
key lookup picks a shard, and each executor holds a connection pool per shard
instead of one.

Harness notes for whoever runs this next:

  - `bin/mx-control-up.sh` (re)starts the control servers on hog1-hog8. The
    remote login shell starts one directory above the checkout, so the binary
    needs an explicit `cd`; a wedged control server shows up as an executor that
    never becomes ready, not as an error.
  - The runner prints every result twice, once through the logger to stderr and
    once plain to stdout. `mx-summary.py` anchors its patterns at the start of
    the line so each trial is counted once.
  - `-a 100` keeps these comparable with the first single-shard sweep, but it is
    a tiny, concentrated working set: ERC20 balances live under one contract
    address, so most state sits on one shard whatever `-d` says. `bin/mx-sweep2.sh`
    takes `NACC` and `SUFFIX` for a larger-keyspace variant, which is the obvious
    next run.

### The same sweep at a wider keyspace (100,000 accounts)

`-a 100` leaves the database nearly free, so the sweep above mostly measures
what sharding costs the executors. Re-running it with a keyspace three orders of
magnitude wider moves the bottleneck: close+commit goes from ~48 ms per block to
~360 ms, and one shard stops scaling with executors (475k -> 515k from two to
three, against 717k -> 1.03M at 100 accounts). Commit, not execution, is the long
pole here.

100k is the ceiling, not a choice. Genesis travels as a single RPC message and
`-a 1000000` dies on it:

    terminate called after throwing an instance of 'x23::XdrBadInput'
      what(): DbSched1::dbsched_load_genesis_configuration:
              message size 124000612 exceeds maximum 16777216

At ~124 bytes per account the cap lands near 135k accounts. Anything larger
needs genesis chunked across messages, or a larger `kMaxMessageSize`.

| shards | 1 exec | 2 exec | 3 exec |
|---|---|---|---|
| 1 | 293,627 ± 36,249 | 475,060 ± 49,202 | 515,399 ± 40,605 |
| 2 | 288,022 ± 11,858 | 437,773 ± 15,591 | 516,740 ± 17,133 |
| 4 | 204,872 ± 2,573 | 342,222 ± 6,435 | 452,013 ± 13,658 |

Read this one carefully, because the single-shard spread is wide (±8-10%) and it
is easy to over-read:

  - **One and two shards are indistinguishable here.** The nine cell-by-cell
    comparisons swing from -8% to +9% with no pattern, and only one of them
    separates by more than the two standard deviations combined -- while its
    neighbour at the same executor count leans the other way. A 2-shard arm that
    appears to win by 5% at three executors is noise, not a crossover. I said
    otherwise mid-run and was wrong.
  - **Four shards is consistently worse** (-12% to -30%, tight spreads,
    separated in every cell). The likely reason is placement rather than
    sharding: ERC20 balances all hang off one contract address, so the state
    stays lumpy no matter how many shards exist, and the extra shards buy
    fan-out cost without taking work off the busy one.
  - Taken with the 100-account sweep, no configuration in either shows sharding
    buying throughput on the execution path. What changes with a wider keyspace
    is that the penalty for two shards falls to zero -- consistent with sharding
    paying for itself only once a single database is the thing in the way.

A workload whose keys actually spread -- `storage_filler`, or an ERC20 variant
keyed per holder -- is the experiment that would separate "sharding does not
help" from "this workload cannot be sharded". That is the next thing to run.

## Capacity: the one benefit that does not have to be earned as a rate

hogdb keeps items in memory and has no read-miss path, so the state a
configuration can address is exactly the sum of its shards' budgets. That makes
capacity provisioned rather than measured -- the only question is whether a run
at that size works. At 75 GB per shard, all of these completed:

| shards | provisioned | peak state held | utilisation |
|---|---|---|---|
| 1 | 75 GB | 30.7 GiB | 44% |
| 2 | 150 GB | 30.0 GiB | 22% |
| 4 | 300 GB | 30.0 GiB | 11% |

The ceiling triples; what the workload puts in it does not move. ERC20 transfers
touch a fixed set of balances, so more shards spread the same ~30 GB thinner
rather than holding more of it -- utilisation falls from 44% to 11% because the
headroom grew and the workload did not. (Careful with the per-shard numbers here:
peak per shard is 30.7/18.9/13.3 GiB, and multiplying those by the shard count
overstates the total, because the shards are not equally loaded. The table above
sums each shard's own peak.)

The largest state this campaign ever held is a **single** shard: 259.8 GiB live
at a 300 GB budget (`1shard-300gbcache-01`, COMPLETE, 93% utilisation, reached
via `--preload-items`). 141.6 GiB at 200 GB (`1shard-preload-01`) is the runner
up. Both are unsharded.

The honest limit on the capacity argument is per-host memory, and the cluster is
**not uniform: hog1 has 755 GB, the other ten have 377 GB each** (4.5 TB in
total). `--ram-bytes` does not change that ceiling -- it reserves address space,
not memory. An idle shard given a 256 GB budget shows VmSize 249 GB against
VmRSS 25 MB, so only live items cost anything, and a shard can be given a budget
larger than its host has RAM. It will start, and then die when live state passes
what the host can hold.

So one shard tops out near 350 GB of live state on a typical host, and a
terabyte cannot be held unsharded on this cluster however the budget is set --
which is the demonstration the campaign was missing. It does not need preloading:
state grows at ~415 bytes per transaction (418 B measured at one shard, 409 B at
four), so at the default workload's ~200k tx/s state accumulates at ~290 GiB/h
and a terabyte is about four hours of ordinary load. Disk keeps up: for
fresh-key workloads bytes-written tracks live state at ~1.0x (it was 10x only for
`storage_filler`, which rewrites the same keys), against 377-398 GB free per
host.

The point cannot be made from below either: the 2 GB-per-shard arms crashed,
because a budget too small for the working set exhausts the allocator instead of
spilling (ISSUES #1).

Plot and per-run table are in `sharding-vs-not.html`, generated from
`results/capacity.json` (written by the extraction in this session; peak state is
the max of hogdb's own `mem stats: X GiB (N% memory items)` lines, summed across
shards).

### The default workload at 300 GB per shard (`*-bigcap`, 2026-08-20)

The capacity ladder above varies the workload as well as the budget, which makes
it hard to read. This one varies only the budget: the default-workload matrix's
parameters exactly -- 100 x 1M transactions, the stock 10,000-account keyspace,
no virtual recipients, no preload, no filler -- with `--ram-bytes` raised from
10 GB to 300 GB per shard.

| shards | provisioned | held | utilisation | elapsed |
|---|---|---|---|---|
| 1 | 300 GB | 2.42 GiB | 0.87% | 344 s |
| 4 | 1,200 GB | 2.05 GiB | 0.18% | 350 s |

A terabyte of capacity, provisioned and verified, on the loadgen as it ships and
in under six minutes. Nothing was preloaded and nothing was filled.

The reason it is cheap: `--ram-bytes` reserves address space, not memory. Each
shard in the 4-shard arm showed **291 GB of VmSize against 2.3 GB of RSS**, and
its host still had 258-489 GB free. An idle shard given a 256 GB budget sits at
VmSize 249 GB / VmRSS 25 MB. So a big budget costs nothing until items arrive,
and the ceiling that matters is not the flag -- it is the host's memory.

Which is what sharding multiplies, and the third line on the plot: hog1 has
755 GB and every other host 377 GB, so one shard tops out near 755 GB and four
reach ~1.9 TB. The full cluster is 4.5 TB across 11 hosts. A budget set above a
host's RAM would start happily and then die when live state passed what the host
could hold, so 300 GB per shard is the largest figure here that is backed by
physical memory rather than address space.

Two measurement notes:

  - hogdb logs its in-memory share as an **integer percent** of the arena, so a
    couple of gigabytes against a 300 GB arena reads as `0% memory items` and the
    figure disappears. `capacity-extract.py` falls back to bytes-written and
    records which source each number came from (`held_source`). For this workload
    that is an upper bound, not a match: the same 10,000 balances are rewritten,
    so writes exceed live state.
  - `hogdb-*-metrics.txt` is still empty in every run -- the harness scrapes it
    after teardown. Fixing that would give an exact resident figure and remove the
    need for the fallback.

What this ladder does not show is sharding being *necessary*. The workload holds
2 GiB; one shard on one host could hold it 150 times over. Necessity needs a
working set past a single host's memory, and the plain loadgen reaches state only
through fresh keys at ~415 B per transaction -- measured at 110 GiB/h in the
aborted `4shard-tb` arm, so roughly nine hours to a terabyte of live state.
That run is the one to do if the question becomes "must you shard", rather than
"how far does the ceiling go".
