# ObjecTier on ramdb — where this branch stands

Branch `vp/obj`. Written 2026-08-21 as Vinita's internship wrapped up, as a front
door to the two long documents already here:

- **[TODO.md](TODO.md)** — what was measured, item by item, with the numbers and
  the four allocator bugs found along the way. 1,345 lines; the status table at
  the top is the index.
- **[PLAN.md](PLAN.md)** — what to do next and why, including a component-by-
  component cross-check against the only other ObjecTier implementation
  (`utah-scs/hemem`, branch `vp/flexkv_varlen`, `apps/flexkvs`).

This file adds only what those two cannot say about themselves: the branch's
state as a piece of repository history, and what someone picking it up should do
first.

## What this is

An implementation of ObjecTier — popularity-driven object consolidation — inside
ramdb's loglet allocator, plus the measurement harness that scored it. The
paper's own §6 says their implementation was never completed; this one is, and it
works:

- `ItemAllocator::consolidate()` selects victim loglets by mixedness and routes
  relocation by temperature.
- Fast-tier hit rate **0.698 → 0.896** at θ=0.99 — 2.91× fewer slow-tier
  accesses — for 37% of the relocation work (107 of 290 loglets, 214 MiB).
- The classifier is a 16 MiB count-min `HeatSketch` at precision 0.92 / recall
  1.00 against an oracle, O(1) in item count, sitting *above* the block lock
  cache.
- The trigger converges in 13 closes and then stops, is bounded per pass, and is
  wired into the production commit path (off by default), verified on
  `hog-devnode`.

**The caveat that governs how any of this may be quoted:** no number here was
measured on tiered hardware. Every AMAT figure is
`hit × 70 + (1 − hit) × 300` on a single-NUMA-node machine. The *layout*
improvement is measured; its translation into time is a projection. Lead with hit
rate, and never quote a nanosecond figure without saying what model produced it.
The one exception is P3b, which emulates a smaller fast tier by explicit eviction
and measured a 2.98× miss ratio — confirming the hit-rate model exactly.

## State as a branch

- 31 commits, ~8,900 insertions across `hogd/ramdb`, `hogd/hogdb`, `hogd/base`,
  and `docs/objectier`.
- Branched off `main` and now roughly 123 commits behind it. It has **not** been
  rebased, and it does not need to be to be read or re-run.
- **Self-contained**: every submodule is on upstream `main`, so unlike the
  sharding branches it clones and builds with no pinning or protocol dependency.
  See [docs/sharding/BRANCHES.md](../sharding/BRANCHES.md) on the archive branch
  for that contrast.
- Not a merge candidate as it stands. It is a correct *mechanism* with the policy
  layer deliberately left out, plus a benchmark harness that is much larger than
  the mechanism. Landing it would mean choosing what part is product.

## If you pick this up

Read PLAN.md's "Recommended order" section; it is current. The short version:

1. **Drift-driven aging.** The top open item. A fixed aging epoch makes the
   trigger relocate on every block forever; no aging at all makes it go silent
   while the layout rots — and, worse, *silently*, which is how P1.1 was found.
   Neither this implementation nor flexkvs has the middle ground. It is also the
   cheapest item on the list.
2. **Tier-aware allocation (C1).** Still todo. Placement is currently external.
3. **Anything that reduces the modelling caveat above.** P3a was abandoned for a
   structural reason — no resident-set cap both survives setup and binds on reads
   — so the file-backed heap (`evict_region()`, `ru_majflt` in `report_faults`)
   is the only tiering signal available on this hardware. It prices the slow tier
   in page faults instead of nanoseconds.

Four traps from TODO.md worth repeating, because each one cost real time:

- **Instrumentation placement is load-bearing.** Counting hotness one layer too
  low produced a confident, entirely wrong answer that only an item-size sweep
  exposed. Any new counter needs the same cross-check.
- **Check exit codes, not summary lines.** BUG-3 aborted the test process at
  teardown for several commits while every test reported passing.
- **The oracle is not a ceiling** — it is optimal only for its own threshold. The
  rank-sorted prefill is the real bound.
- **Tier capacity dominates.** The benefit is 2.4× larger at a 5% fast tier and
  zero at 50%. Three assumed constants (fast latency, slow latency, capacity) and
  this is the one that matters.

## Running it

The harnesses and their plotting scripts are in this directory: `run_v1.sh`,
`run_p2.sh`, `run_x3.sh`, `run_ssd_tier.sh` and the matching `plot_*.py`.
Figures land in `figures/`, data in `data/`. The `ramdb_bench` and `hogdb_bench`
commands each script drives are named in TODO.md's "Harness notes" section.
