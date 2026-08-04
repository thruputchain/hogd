# ObjecTier in ramdb — forward plan

Companion to `TODO.md`, which records what has been measured. This is what to do
next and why. Written after V1/B1/B2 landed; revised after the flexkvs
cross-check.

---

## Cross-check: the flexkvs implementation

There is a second ObjecTier implementation to compare against —
`utah-scs/hemem`, branch `vp/flexkv_varlen`, `apps/flexkvs`. The live binary is
`kvsbench`: `bench/main.c` linked against the server's `hashtable.c` and
`ialloc.c`. It is the same idea in the same place (a segment allocator with two
relocation destinations chosen by temperature) and it made the opposite
trade-off: it is a *system* with a weak classifier, where ramdb is a correct
*mechanism* with no policy.

| §5 component | flexkvs | ramdb |
|---|---|---|
| Classifier | `uint16_t count` in `struct item`, bumped under the bucket lock on GET | `HeatSketch`, off-header, fed above the block lock cache |
| Threshold | adaptive: 100 bucket samples/ms, 80th percentile, EWMA α=0.01 | adaptive: exact percentile of the sketch's own histogram (was a fixed `--hot-hits`; see P1.2) |
| Victim selection | `\|1 − 2·hot_ratio\|`, min-rank, gated `< 0.6`, **one segment per pass** | `min(hot_frac, 1−hot_frac)`, **every** loglet over threshold, batch |
| Relocator | `ialloc_alloc(…, cold)` → per-worker `cold`/`cur` segments | `compact_one(src, &is_hot, dests)` → allocator-owned `CompactDests` |
| Policy manager | maintenance thread, 1 ms, incremental cleanup queue | `consolidate_incremental()` at a quiescent point, bounded (see P2.1); production call site still open |
| Aging | counts reset on relocation and on update only — non-uniform, and a bug | `halve()` on a caller-chosen epoch; required, see P1.1 |
| Tier-aware alloc | none; external placement (`numactl -m1`/`-m2`) | none (C1) |
| Metric | throughput + p50…p99.99, plus 2 MB-page hit rate from an access trace | fast-tier hit rate from allocator introspection, modelled AMAT |

**What flexkvs has that this plan wants.** Three of the items below stopped
being speculative because there is a working reference for each: an adaptive
percentile threshold (1.2), a trigger with incremental bounded relocation
(2.1/2.3), and a moving hot set (`workload_init_dyn`, `BENCHMARK_DYN_HOTSET`) for
1.1. All three are now done here, and each one turned up something the reference
does not show: the threshold optimizes at the fast tier's own size, the trigger
converges and then terminates, and un-aged tracking fails *silently* rather than
gradually. What remains from that list is real tiered hardware, which is Phase 3.

**What ramdb has that flexkvs does not, and should keep.**

- **Hotness survives relocation.** `hasht_put` sets `nit->count = 1`
  unconditionally and `clean_log` relocates through it, so every relocation
  zeroes the hotness of the object it just moved: an object relocated twice is
  classified cold the second time. A key-hashed sketch is invariant under
  relocation by construction. This is the strongest argument for the off-header
  design and it should be stated as such, not just as a space argument.
- **Counting does not dirty the object.** `count += 1` writes into the item
  header — into the very memory being tiered — turning every read of a cold
  object into a write to the slow tier. The sketch is DRAM-resident and O(1).
- **The victim metric does not degenerate.** `|1 − 2·hot_ratio|` is the paper's
  literal wording; it works for flexkvs only because its hot set is 25%. At the
  2.2% hot set measured here every loglet scored 0.044 (see `loglet.h`).
- **Ownership and concurrency.** `UniqueLoglet`, the `modify_locked` pointer
  re-check, reserve-exhaustion deferral. flexkvs splices hot and cold segments
  into one GC list through a shared `ia->prev`, and the resulting list order
  under interleaved rollovers is undefined.

**Both hit the same in-place-update wall.** `VAR_VALUE_SIZE` is `0` on that
branch, so a SET to an existing key updates in place and never moves the item —
independently the same finding as D2 here. Worth citing: two unrelated stores,
same mechanism, same conclusion that fragmentation-driven cleaning cannot fix
the layout.

---

## Where this stands

**Working, measured, committed:**

- `ItemAllocator::consolidate()` — victim selection by mixedness, relocation
  routed by temperature. First end-to-end ObjecTier implementation; the paper's
  own §6 says theirs was never completed.
- Fast-tier hit rate 0.698 → 0.896 at θ=0.99, i.e. 2.91× fewer slow-tier accesses.
- Full benefit for 37% of the relocation work (107 of 290 loglets, 214 MiB).
- A practical classifier: 16 MiB count-min sketch, precision 0.92 / recall 1.00,
  matching an oracle. Space is O(1) in item count.
- Three allocator bugs found and fixed along the way (BUG-1/2/3).
- Since the flexkvs cross-check: bounded victim claiming with a `max_victims`
  cap, write-head rollover (128 previously unreachable loglets per pass), and a
  threshold the sketch derives from its own count distribution (P1.4, P1.2).

**The one sentence that should worry us:** *no number in any of this was measured
on tiered hardware.* Every AMAT figure is `hit × 70 + (1 − hit) × 300` on a machine
with one NUMA node. The layout improvement is real and measured; its translation
into time is a projection. That is the first thing a reviewer will attack, and
Phase 3 is the only thing that answers it.

---

## Phase 1 — make the existing claims defensible

No new hardware. Everything here closes a hole a reviewer would find.

### 1.1 Aging — the classifier is unproven under a moving hot set

`HeatSketch::halve()` exists and has never been called, so the sketch currently
records "was ever hot". Every workload we run has a *static* hot set, which is
exactly the case where that distinction does not matter — so the classifier is
untested in the only regime where aging exists.

- Add a shifting-hot-set workload: `zipf_payments` with the rank→slot permutation
  re-drawn every K blocks, so the hot accounts change identity mid-run. flexkvs's
  `workload_init_dyn` / `BENCHMARK_DYN_HOTSET` is the same shape — one hot-set
  redraw at a configured time — and is the cheaper thing to copy first.
- Call `halve()` on an epoch (start with: once per N blocks, swept).
- Measure hit rate against epoch length, with and without aging.
- Note flexkvs has an *accidental* aging scheme — counts reset on relocation and
  on update, never otherwise — so relocated objects age and untouched ones
  accumulate forever. That is a bug there, but it makes the point that the epoch
  has to be uniform across objects to be interpretable.
- **Done when:** there is a curve showing un-aged tracking degrading as the hot set
  moves, and an epoch that recovers it. If aging turns out not to matter even
  under a moving hot set, delete `halve()` rather than ship dead code.
- **Status: done, see P1.1 in `TODO.md`.** `halve()` stays. Un-aged tracking does
  not degrade gracefully -- four blocks after the hot set moves the trigger
  selects nothing and the layout freezes at a hit rate within half a point of the
  scattered baseline, reporting no work to do. Halving every 2 blocks recovers the
  full 0.9045 in ~11 blocks. `hogdb_bench -E K` moves the hot set, `-G K` ages.
- **Now the open question is the epoch, not whether to age.** A fixed period makes
  the trigger relocate on every block forever, even while it is only holding a
  layout it already reached. The epoch wants to be driven by detected drift so a
  static hot set costs nothing; that is unimplemented and is the natural successor
  to this item.

### 1.2 Threshold — the oracle was never the ceiling

The 4 MiB sketch *beat* the oracle (0.9055 vs 0.8964) because its collisions made
it over-inclusive, and the oracle's hard cut at ≥2 accesses is arbitrary. The
rank-sorted prefill at 0.9574 is the real ceiling, so ~5 points sit unclaimed.

- Sweep `--hot-hits` at a fixed collision-free sketch size.
- Sweep it jointly with fast-tier capacity: the optimum almost certainly moves
  with tier size, since a tight tier wants a stricter hot set.
- Better than sweeping: derive it. flexkvs samples 100 hash buckets every
  millisecond, takes the 80th percentile of the counts and smooths it — the
  threshold is measured rather than configured. ramdb can do better than
  sampling: the sketch *is* the count distribution, so a histogram over one row
  gives an exact percentile in one pass, with no RNG and no EWMA. The knob then
  becomes "what fraction of tracked keys should be hot", which is the
  operator-facing parameter (fast-tier capacity) rather than a raw count.
- **Done when:** the hot-set threshold is chosen by measurement rather than
  inherited from a default, and the residual gap to 0.9574 is explained.
- **Status: half done.** `HeatSketch::hot_threshold()` + `-F/--hot-fraction`
  landed; see the P1.2 result in `TODO.md`. The threshold is derived now, and it
  optimizes at the fast tier's own size, which is the joint-sweep answer 1.2
  asked for. **The gap to 0.9574 is still unexplained** — that half is open.

### 1.3 Cleaner cost — finish X5 properly

Have: 289 loglets / 5.98 M items / 7.0 s single-threaded ≈ 85 MB/s. The paper's
back-of-envelope is 150 MB/s for a 256 GB heap relocated hourly, never measured.

- Parallelise consolidation across the pool. **Note this reopens BUG-2's reserve
  drain** — two destinations per worker across 128 workers — so it needs the
  reserve accounting revisited, not just a `pool_.submit`.
- Report bytes-moved/s as a share of measured machine memory bandwidth, and CPU
  time as a share of a core.
- **Done when:** there is a measured figure to put beside the paper's 0.06%.
- **Effort:** medium. The parallelisation is the risky part.

### 1.4 Two structural gaps in victim selection

Both are real and both are currently just documented.

- **Active write heads are invisible.** A per-thread head reaches
  `compactable_loglets_` only when it fills, so up to one partly-filled loglet per
  core (~256 MB here) of the *newest* data can never be consolidated. Options:
  force a roll-over before a pass, or let consolidation borrow a head under its
  lock. The first is simpler and probably enough.
- **`consolidate()` claims the whole list.** Between the swap and the restore,
  `compactable_loglets_` is empty, so a concurrent `compact()` sees nothing,
  reclaims zero, and `background_worker` escalates. Harmless for a manually
  invoked pass; a correctness-adjacent bug the moment Phase 2 puts it on a
  trigger. Fix by filtering in place under the lock, or by claiming in bounded
  batches.
- **Done when:** a consolidation pass can run concurrently with compaction without
  either starving the other, and newly written data is reachable.
- **Status: done.** Batch claiming (32 at a time) with a `max_victims` bound, and
  `roll_over_heads()`, which turned up 128 previously unreachable loglets — one
  per core, as predicted. See the P1.4 entry in `TODO.md`. The `max_victims`
  bound is also the mechanism 2.3 needs to interleave rather than barrier.
- **Not verified:** nothing yet *runs* a compaction and a consolidation
  concurrently. The list is no longer emptied, which was the bug, but the
  "without either starving the other" half needs the Phase 2 trigger to be
  testable at all.

---

## Phase 2 — turn a mechanism into a system

`consolidate()` is invoked by hand from a benchmark. The paper's §5 names three
components; we have the Object Classifier and the Object Relocator, and nothing at
all for the **Relocation Policy Manager**. That is the gap between "a mechanism
works" and "a store does this".

### 2.1 Trigger policy

Candidate triggers, in the order I would try them:

1. **On `set_active(false)`** — between ledger closes. Groundhog alternates
   activity and quiescence, and consolidation has no deadline behind it, so
   spending idle time is nearly free. Most likely to be the answer.
   **Blocker found (P2.0): nothing calls `set_active()` outside `db_test.cc`.**
   Its comment describes consolidation's precondition exactly, but hogdb never
   enters the inactive state, so this trigger does not have a signal to hang off
   until the block boundary is wired to it. That wiring is step one, and it is
   also the only place the no-concurrent-write precondition holds for free —
   every other trigger below needs the precondition relaxed first.
2. **Periodic**, on `background_worker`'s existing 1 s timeout tick.
3. **Mixedness-budgeted** — maintain a running estimate of total misplaced bytes
   and fire when it crosses a threshold.

Note compaction today is demand-driven only (`notify_low_mem`), so there is no
existing cadence to attach to. That is why this is design work, not a flag.

flexkvs answers this with (2): a dedicated maintenance thread on a 1 ms tick that
walks every allocator, picks *one* victim segment each time, and enqueues its
items for the worker threads to relocate on the request path. Two things to take
from it — the work per tick is bounded (one segment), and relocation is
interleaved with serving rather than run as a barrier. Two things not to take —
1 ms is far too tight for a ledger's cadence, and putting relocation on the
request path is the p99 hazard 2.3 is about. Groundhog's `set_active(false)` is a
better clock than a timer, and is still option (1).

- **Done when:** consolidation runs unattended and the store reaches a steady
  state, measured over a long run rather than one shot.
- **Effort:** medium-large. This is the largest remaining piece of engineering.

**Status: done in ramdb, see P2.1/P2.2 in `TODO.md`.**
`Database::consolidate_incremental()` plus `hogdb_bench -A N` converges the layout
to hit 0.9045 in 13 block boundaries at 2 loglets each, and then selects nothing
further — so 2.2's convergence question is answered too, and the stop condition
did not have to be invented. Two defects had to be fixed to get there, both found
only by measuring: consolidation abandoned its relocation destinations between
passes, and `min_mixedness = 0` made every pass re-consolidate the output of the
last one.

`set_active()` turned out to be the wrong hook — it resizes the worker pool, and
an asynchronous notification cannot hold the no-concurrent-write promise. The
trigger is synchronous on the committing thread instead.

**What remains is one call site.** `HogDBSchedRpc::commit_block`, after the block
tx shuts down, gated on `RPCContext::block_txes_` being empty — a sibling or
viewing block still holding a reference means the store is not quiescent. That is
a consensus-path change and wants an owner's review rather than a guess.

### 2.2 Steady state, not one shot

Every result so far is "workload, then one consolidation pass, then measure". A
real system interleaves them.

- Long run with continuous churn and triggered consolidation.
- Track hit rate, bytes relocated, and re-consolidation rate over time.
- **The key question:** does the layout converge, or does consolidation keep
  fighting new writes? §6 predicts the former ("very hot and very cold objects
  would rarely be relocated"); the bimodal mixedness distribution supports it, but
  it has not been shown over time.

### 2.3 Tail latency

`compact()` is a barrier pass — claims everything, fans out, waits. Consolidation
is currently serial and unbounded. Both are p99 hazards.

- Measure read p99/p999 with consolidation in flight.
- This is a result the paper cannot produce (memory-only framing) and reviewers of
  a storage paper will want.

---

## Phase 3 — real hardware, the credibility gap

**This is the highest-value phase and the only one that is blocked.** Everything
above refines projections; this replaces them with measurements.

### 3.1 Get a suitable machine

This host is a single-socket EPYC 9554: one NUMA node, no CXL, no Optane, no dax.
There is no slow tier to measure against. Needed, in preference order:

1. **Real CXL or Optane.** Ideal, matches the paper's 70/300 model directly.
2. **Two-socket box** — remote-socket DRAM as the slow tier. ~1.6–2× latency, not
   the 4× of Optane, but it is *measured* rather than modelled, and HeMem, TPP and
   MEMTIS all validate this way, so it is defensible.

**Action: ask what the group has.** This is a procurement question, not an
engineering one, and it gates the strongest result. It should be asked now rather
than after Phase 1, because lead time may be long.

### 3.2 Tier the allocator (C1)

- `mbind()` the fast fraction of the loglet region to the near node, the rest to
  the far node; or map the slow fraction from a dax file.
- Split `free_loglets_` into fast and slow free lists; `alloc(mode, tier)`.
- Route consolidation's hot destination to the fast list.
- **Effort:** medium. The allocator is one contiguous mmap, which makes this
  tractable — that was the original argument for ramdb as the vehicle.

### 3.3 Measured AMAT

- `perf mem` / PEBS load-latency sampling, scattered vs consolidated.
- **Compare the measurement against the model.** If measured AMAT tracks
  `hit × fast + (1 − hit) × slow`, that retrospectively validates every modelled
  number in Phases 1–2 and is itself a contribution. If it does not, we need to
  know which of the three assumptions was wrong.
- Watch for the confound already identified: bucket-lock contention rises with
  skew and can mask the memory-latency win. Separate lock wait from memory stall.

### 3.4 Loglet size (A2) — needs root

`kLogletRegionSize` is 2 MiB and huge pages are not reserved on this host
(`HugePages_Total: 0`), so TLB numbers are currently meaningless. X3 showed ramdb
sits deep in the many-items-per-tiering-unit regime; A2 is what would traverse the
interesting part of that curve, by shrinking the *unit* rather than growing the
item. Requires `sysctl vm.nr_hugepages` — needs root on whatever machine 3.1
lands on.

---

## Phase 4 — external validity

### 4.1 Skewed traffic through the real stack

The Groundhog loadgen picks accounts with `std::uniform_int_distribution` and
`ERC20PaymentsConfig` has no skew field, so the deployed workload is θ=0 — the one
place the paper itself says consolidation is pointless. Two options:

- **Add a skew field** to `loadgen.x` and the generator. Protocol change, with
  submodule and groundhog-go propagation.
- **Argue `hogdb_bench` suffices.** It drives the real hogdb storage path with real
  transactions; consensus and mempool are irrelevant to a memory-layout question.

I lean to the second, and to stating plainly that the shipped loadgen is uniform.
Do the protocol change only if a reviewer demands the full stack.

### 4.2 Where does real traffic actually sit?

Two parameters decide whether any of this matters in production, and both are
measurable from traces rather than argued:

- **Skew (θ).** Panel (a) shows the benefit vanishing below θ≈0.3.
- **Read/write correlation (ρ).** Payments read and write the same accounts, so
  ledgers sit at ρ≈1 — the regime where a log-structured store partly
  self-consolidates. But balances update *in place*, so nothing relocates and the
  layout is frozen at creation, which is why consolidation still pays.

**Done when:** Groundhog is placed on both curves from real traffic, rather than
assumed.

---

## Recommended order, and what to cut

1. **Ask about hardware now** (3.1) — long lead time, gates the best result. Note
   flexkvs runs on a box with a real NVM node (`numactl -m2`); if that machine is
   reachable, Phase 3 is a scheduling question rather than a procurement one.
2. **Phase 1.4** — bounded claiming and write-head rollover. Moved to the front:
   everything else here either triggers consolidation or measures it, and both
   are unsound while a pass empties the compactable list. ← *in progress*
3. **Phase 1.2** — the adaptive threshold, as a histogram over the sketch rather
   than a sweep. Cheap, and it deletes an assumed constant. ← *in progress*
4. **Phase 1.1** — aging under a moving hot set.
5. **Phase 2.1 and 2.2** — the trigger and steady state. This is what makes it a
   system rather than a demo.
6. **Phase 3** as soon as hardware exists; it displaces everything else.
7. **Phase 4.2** — cheap, and it decides whether the deployed system cares.

The file-backed heap now on the branch (`evict_region()`, `ru_majflt` in
`report_faults`) is a third option for 3.1 that needs no hardware: it prices the
slow tier in page faults rather than nanoseconds. It will not reproduce a 70/300
model, but it turns "layout changes the fault count" into a measurement on this
host, and it is the only tiering signal available until 3.1 resolves.

**Cut unless a reviewer asks:** 4.1's protocol change, and 1.3's parallelisation if
the serial cost figure is already low enough to make the point.

**Do not skip 1.1.** A classifier that has only ever been tested against a hot set
that never moves is not a classifier anyone should trust, and it is the cheapest
item on this list.

---

## Standing risks

- **Everything is modelled** until Phase 3. Lead with hit rate, not nanoseconds.
- **The oracle is not a ceiling.** It is optimal only for its own threshold; the
  rank-sorted prefill is the real bound.
- **Three assumed constants** — fast latency, slow latency, tier capacity. Capacity
  matters most: the benefit is 2.4× larger at a 5% tier and zero at 50%.
- **Instrumentation placement is load-bearing.** Counting hotness one layer too low
  produced a confident, entirely wrong answer that only a size sweep exposed.
  Any new counter needs the same check.
- **Check exit codes, not summary lines.** BUG-3 aborted the test process at
  teardown for several commits while every test reported passing.
