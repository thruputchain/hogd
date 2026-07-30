# ObjecTier in ramdb — forward plan

Companion to `TODO.md`, which records what has been measured. This is what to do
next and why. Written after V1/B1/B2 landed.

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
  re-drawn every K blocks, so the hot accounts change identity mid-run.
- Call `halve()` on an epoch (start with: once per N blocks, swept).
- Measure hit rate against epoch length, with and without aging.
- **Done when:** there is a curve showing un-aged tracking degrading as the hot set
  moves, and an epoch that recovers it. If aging turns out not to matter even
  under a moving hot set, delete `halve()` rather than ship dead code.
- **Effort:** small. The permutation and the sketch both exist.

### 1.2 Threshold — the oracle was never the ceiling

The 4 MiB sketch *beat* the oracle (0.9055 vs 0.8964) because its collisions made
it over-inclusive, and the oracle's hard cut at ≥2 accesses is arbitrary. The
rank-sorted prefill at 0.9574 is the real ceiling, so ~5 points sit unclaimed.

- Sweep `--hot-hits` at a fixed collision-free sketch size.
- Sweep it jointly with fast-tier capacity: the optimum almost certainly moves
  with tier size, since a tight tier wants a stricter hot set.
- **Done when:** the hot-set threshold is chosen by measurement rather than
  inherited from a default, and the residual gap to 0.9574 is explained.
- **Effort:** small; both flags exist, it is a sweep.

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
- **Effort:** medium, and 1.4b should land *before* Phase 2.

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
2. **Periodic**, on `background_worker`'s existing 1 s timeout tick.
3. **Mixedness-budgeted** — maintain a running estimate of total misplaced bytes
   and fire when it crosses a threshold.

Note compaction today is demand-driven only (`notify_low_mem`), so there is no
existing cadence to attach to. That is why this is design work, not a flag.

- **Done when:** consolidation runs unattended and the store reaches a steady
  state, measured over a long run rather than one shot.
- **Effort:** medium-large. This is the largest remaining piece of engineering.

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

1. **Ask about hardware now** (3.1) — long lead time, gates the best result.
2. **Phase 1.1 and 1.2** — small, and they close the two most obvious holes in the
   classifier story.
3. **Phase 1.4b** — the concurrent-compaction bug, before anything triggers.
4. **Phase 2.1 and 2.2** — the trigger and steady state. This is what makes it a
   system rather than a demo.
5. **Phase 3** as soon as hardware exists; it displaces everything else.
6. **Phase 4.2** — cheap, and it decides whether the deployed system cares.

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
