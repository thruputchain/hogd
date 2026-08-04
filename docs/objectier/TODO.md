# ObjecTier in ramdb — implementation tracker

Branch: `vp/obj`. Companion docs: `PROPOSAL.md`, `CODE_SKETCH.md` (not yet committed).

Applying ObjecTier (ICPE Companion '25, doi:10.1145/3680256.3721319) to `hogd/ramdb`.

**Framing:** ObjecTier was never implemented end-to-end — paper §6 states the memcached
integration is incomplete, Figs 2/3 are simulations of a synthetic array, and Fig 5's 60%
is a *static* pre-arranged-layout A/B with no classifier, no relocation, and no cleaner
cost. A working ramdb version is the **first implementation**, on the application class
§6 names as future work ("row-store, OLTP-focused in-memory databases ... scattered hot
and cold objects primarily under a single index").

---

## Status

| ID | Item | State |
|----|------|-------|
| H1 | Zipf key generator + rank-space keys in `db_bench` | **done** |
| H2 | Consolidated vs scattered prefill layouts | **done** |
| H3 | Fixed-thread-count option (skip the 1..128 core sweep) | **done** |
| H4 | Reserve 2 MB huge pages on the test host | blocked — needs root |
| T1 | NUMA-emulated slow tier (`mbind` / `numactl`) | **next** — gates X2/X4 |
| X2 | Static-layout ceiling (ramdb's Fig 5) | pilot run, null in flat DRAM |
| X4 | Measured skew sweep (ramdb's Fig 3) | pilot run, null in flat DRAM |
| X1 | Per-loglet utilization CDF (ramdb's Fig 2) | **done** — replicates the paper |
| B1 | Temperature-routed `compact_one` | todo |
| V1 | Per-`Loglet` hot metadata + mixedness victim selection | **done** |
| B1 | Temperature-routed `compact_one` | **done** (folded into V1) |
| — | **First working ObjecTier: 139.3 → 86.6 ns, 88% of the gap** | see below |
| H5 | Churn workload that actually triggers compaction | **built** — but see BUG-1 |
| BUG-1 | `LogletAllocator::alloc` segfaults when the reserve drains | **fixed** |
| BUG-2 | Compaction reserve is structurally marginal (heavy deferral) | **fixed** |
| B2 | Real classifier scored against the oracle | **done — works, at the right layer** |
| BUG-3 | `consolidate()` freed loglets through a dangling allocator at exit | **fixed** |
| X5 | Measured cleaner cost vs the 0.06% estimate | unblocked |
| C1 | Tier-aware `LogletAllocator` | todo |
| X3 | Item-size sweep (tests paper §4.3's untested prediction) | **done** — prediction holds |
| P1.4 | Bounded victim claiming + write-head rollover | **done** |
| P1.2 | Adaptive threshold from the sketch's count distribution | **done** |
| P2.0 | Quiescence budget: cost per loglet vs. the available window | **done** |
| P2.1 | Trigger policy — bounded pass at every block boundary | **done** (ramdb + bench; production call site open) |
| P2.2 | Steady state — does the layout converge? | **done — converges in 13 closes, then stops** |
| P1.1 | Aging under a moving hot set | **done — un-aged tracking fails silently; halve() stays** |
| BUG-4 | Trigger default overridden by a DAO wrapper's own default | **fixed** |
| P2.3 | Trigger wired into the production commit path | **done — verified on hog-devnode** |
| P3a | SSD-backed slow tier via a resident-set cap | **abandoned — no cap both survives setup and binds on reads** |
| P3b | Emulated fast tier by explicit eviction | **done — 2.0x fewer device reads, measured** |

### What landed

`db_bench.cc` gains a Zipf rank generator (Gray et al., YCSB-style, O(1) per draw,
θ=0 degenerates exactly to uniform), a fixed rank→key mapping, and two commands:

    ramdb_bench <db> read_zipf_consolidated <num> <value_size> [theta] [threads]
    ramdb_bench <db> read_zipf_scattered    <num> <value_size> [theta] [threads]

Both hold the same key set and the same read stream; they differ only in insertion
order. Because the allocator appends, insertion order *is* memory order — so inserting
in popularity order produces the consolidated layout with no consolidator running. This
is the Fig 5 methodology, and it gives the zero-relocation-cost ceiling that later
results should be reported as a fraction of.

### Pilot: static-layout A/B, flat DRAM

50 M items × 64 B, 16 threads, ops/s:

| θ | scattered | consolidated | delta |
|---|-----------|--------------|-------|
| 0 | 30.2 M | 29.7 M | −1.4% |
| 0.5 | 23.0 M | 22.9 M | −0.8% |
| 0.9 | 27.0 M | 25.1 M | −7.2% |
| 0.99 | 29.8 M | 28.5 M | −4.4% |

**Null, as it should be.** With no slow tier every access is DRAM, so there is nothing
for consolidation to win — the paper's entire gain comes from the fast/slow gap. This is
a useful control: it establishes that layout alone costs nothing (within a few percent),
so any gain measured after T1 lands is attributable to tiering rather than to the
reordering itself. Do not report these as a failed replication.

The consolidated side being consistently a hair *slower* is unexplained and probably
noise or cache-set aliasing; recheck once T1 gives a real signal, and don't chase it
before then.

Also note θ=0.99 outperforms θ=0.5 — high skew keeps the hot set cache-resident. The
non-monotonicity is a cache effect, not a tiering effect, and will invert once tiered.

**T1 is the gate.** Neither X2 nor X4 produces a meaningful number until part of the
loglet region is backed by slower memory.

---

## X1 result — Figure 2 replicated on a real store

50 M items × 64 B, θ=0.99, 32 threads, 3 accesses/item (150 M accesses), 2304 occupied
loglets (4.8 GB). Raw CDFs in `data/x1_{scattered,consolidated}.csv`; figure in
`figures/x1_layout.{pdf,png}`, regenerated by `python3 plot_x1.py`.

Draft caption: *ramdb per-loglet layout under a Zipf θ=0.99 read workload. (a) CDF of
hot-byte utilization — the fraction of each 2 MB loglet that was accessed at least once.
Scattered is near-vertical: popularity-oblivious placement gives every loglet the same
mix. Consolidated spreads, lifting the fast tier (top 20%) to 0.57–1.00. The dotted rule
marks live-byte utilization, identical under both layouts — what ramdb's compactor
optimizes today. (b) AMAT modelled at 70 ns fast / 300 ns slow, 2.05× apart.*

    ramdb_bench <db> layout_scattered    50000000 64 0.99 32 3
    ramdb_bench <db> layout_consolidated 50000000 64 0.99 32 3

| | scattered | consolidated | paper |
|---|---|---|---|
| mean hot-byte util, all loglets | 0.392 | 0.392 | — |
| mean **live** util, all loglets | 0.993 | 0.993 | — |
| **hot-byte util of the fast tier** | **0.393** | **0.815** | ~0.50 → 0.70–1.00 |
| fast-tier hit rate (top 20%) | 48.9% | 90.6% | — |
| **modelled AMAT** | **187.4 ns** | **91.6 ns** | 167 → 87 ns |

AMAT ratio **2.05×**, against the paper's claimed 2×. Fast-tier hot utilization under the
scattered layout is 39% — i.e. **61% of the fast tier is cold data**, worse than the
paper's simulated ~50%.

CDF shape matches Fig 2a qualitatively: scattered is near-vertical (0.391→0.405 between
the 20th and 100th percentile — random placement gives every loglet the same mix), while
consolidated spreads 0.08→1.00 with the top 20% spanning 0.57–1.00.

**The money statistic is the two mean rows.** Live utilization 0.993 against hot
utilization 0.392, on identical data: liveness and popularity are almost entirely
uncorrelated. That is the gap the whole project targets, now measured rather than argued.

Self-consistency check that held: mean hot utilization is *identical* across layouts
(0.3916 both). It must be — same touched byte total over the same loglet count. Only the
distribution moves. If these ever diverge, the analysis is buggy.

---

## H5 result — ramdb already captures ~91% of the benefit, implicitly

This is the most consequential measurement so far and it cuts against the headline.

All four rows at 10 M items, 64 B, θ=0.99, 16 threads, 80 M accesses — matched, so
directly comparable. Churn rows ran 8 overwrite rounds at 1.5× heap first.

| layout | fast-tier hot util | hit rate | live util | AMAT |
|---|---|---|---|---|
| prefill scattered (artificial floor) | 0.676 | 0.450 | 0.987 | **196.4 ns** |
| churn from scattered | 0.608 | 0.858 | 0.724 | **102.6 ns** |
| churn from consolidated | 0.664 | 0.859 | 0.726 | **102.4 ns** |
| prefill consolidated (oracle ceiling) | 0.975 | 0.898 | 0.987 | **93.4 ns** |

**The two churn rows converge.** 102.6 vs 102.4 ns, hit rate 0.858 vs 0.859 — the
starting layout is almost entirely washed out. Under realistic churn ramdb lands in the
same place whether it began perfectly sorted or perfectly shuffled.

And that place is close to the ceiling: `(196.4 − 102.5) / (196.4 − 93.4)` = **91% of the
available AMAT gap is already captured**. Remaining headroom for any consolidator is
102.5 → 93.4 ns, i.e. **1.10×**, not the 2.05× the prefill-only X1 figure implies.

**Why:** a log-structured store appends writes in time order, so a loglet's items share a
write epoch. Hot keys get overwritten and move to the current write head; cold keys stay
behind. Loglets therefore stratify by age, and age correlates with hotness — and
compaction gathers survivors (the cold ones) into separate destinations. The store is
doing temporal segregation as a side effect of being log-structured.

The paper anticipates exactly this, for a different runtime — §6: *"Generational garbage
collectors already perform a form of hot and cold separation. One area of future work we
plan to investigate is the extent to which these runtimes already help perform similar
consolidation to what ObjecTier does."* A log cleaner is the same phenomenon, and this
measurement answers that question with a number.

### The confound that has to be resolved next

`churn_items` overwrites **Zipf-selected** keys, and `read_items_zipf` reads with the
same distribution and the same generator. So write-recency is a near-perfect proxy for
read-hotness *by construction*, which is the best possible case for implicit temporal
segregation. The 91% is therefore an upper bound on how much ramdb gets for free.

The experiment that settles it: **decorrelate the read and write distributions** — hot
reads over cold-written keys. If implicit segregation collapses there, ObjecTier's
read-hotness classifier earns its keep and B1 has real headroom. If it holds up, the
honest paper claim is that log-structured stores mostly solve this already, which is a
publishable negative result and a direct answer to the paper's own §6 question.

Do this before building V1/B1. It decides whether they are worth building.

### Resolved: the confound was the entire effect

`churn_decorrelated` changes exactly one thing — the overwrite stream draws from its own
rank space through a separately seeded shuffle, so the most-written keys are unrelated to
the most-read ones. Same size, skew, thread count, rounds, heap, and access total.

| | hit rate | AMAT | share of gap captured |
|---|---|---|---|
| prefill scattered (artificial floor) | 0.450 | 196.4 ns | — |
| **churn, decorrelated writes** | **0.481** | **189.3 ns** | **7%** |
| churn, correlated writes | 0.858 | 102.7 ns | 91% |
| prefill consolidated (oracle ceiling) | 0.898 | 93.4 ns | 100% |

Implicit segregation **collapses**: 189.3 ns is within 4% of the artificial floor. So the
91% figure was almost entirely an artifact of drawing reads and writes from one
distribution, and ramdb's log structure buys essentially nothing once read hotness stops
tracking write recency.

Headroom for a consolidator in the decorrelated regime: 189.3 → 93.4 ns = **2.03×** —
which is the paper's claimed 2×, now on a real store with a defensible baseline.

**The real contribution is the conditioning variable, not a single number.** Consolidation
is worth 1.10× or 2.03× on the *same store under the same skew*, depending only on
read/write correlation. Nothing in the paper distinguishes these regimes.

### This invalidates the `lsn`-as-classifier plan

Earlier design note recommended `ItemHeader::lsn` as a free hotness signal, on the
grounds that it is information the store already keeps (paper §4.1). That reasoning only
holds in the correlated regime. Where it holds, ramdb needs no classifier at all — it is
already at 91%. Where a classifier is actually needed, `lsn` carries no read-hotness
information by construction. **The free proxy is useful exactly where it is unnecessary.**

So B2 (the classifier study) is back to being load-bearing rather than a sensitivity
analysis, and it has to be a genuine *read*-hotness signal — bit-stolen counter, CLOCK
bits, or the off-header sketch. Which reopens the three blockers listed below.

### D2 answered: a ledger's layout is frozen at creation

Measured on the real hogdb path via `hogdb_bench zipf_payments` (2 M accounts,
θ=0.99, 290 occupied loglets):

| | fast-tier hot util | hit rate | modelled AMAT |
|---|---|---|---|
| hot payees scattered | 0.204 | 0.699 | **139.3 ns** |
| hot payees sorted (ceiling, `--sorted`) | 0.304 | 0.957 | **79.8 ns** |
| θ=0 scattered (control) | 0.594 | 0.546 | 174.4 ns |

**1.75× of headroom — and it is permanent.** Replaying the payment set 40 times
produced a byte-identical layout line and **zero compaction passes**. Items never
moved.

The reason is [scalable_map.cc:212-221](../../hogd/ramdb/scalable_map.cc#L212-L221):
a `put` whose value size is unchanged updates **in place** — no allocation, no
tombstone, no log growth. Account balances are `int128_xdr`, a fixed 16 bytes, so
*every* payment takes that path. Payment traffic therefore never fragments the
item log, never pressures memory, and never wakes the compactor.

**This inverts the earlier reasoning.** The implicit self-consolidation measured
in H5 depended on *size-changing* overwrites forcing realloc, which is what moved
hot items to the write head and stratified loglets by age. Fixed-width balance
updates bypass that mechanism entirely. So for a ledger:

- the layout is whatever the initial write order produced, permanently;
- the 1.75× gap is not a transient to be washed out, it is the steady state;
- compaction cannot fix it, because compaction never touches these items;
- **relocation driven by read popularity is the only mechanism that can** — which
  is precisely what ObjecTier proposes.

That makes the ledger a *stronger* case for ObjecTier than the synthetic churn
experiments suggested, not a weaker one. It also means B1 alone is insufficient
here: routing relocations by temperature only helps items the compactor visits,
and it visits none of these. A ledger needs consolidation triggered by
*mixedness* (V1) independent of fragmentation — the paper's §4.2 victim
selection, not ramdb's liveness-driven trigger.

### V1+B1 result — ObjecTier works, on a real store

`ItemAllocator::consolidate()` selects victims by **mixedness** and relocates their
items into two temperature-homogeneous destinations. Driven from
`hogdb_bench zipf_payments -C` with an *oracle* classifier built from the generated
access distribution, so this measures the relocation mechanism independently of any
hotness signal's accuracy.

2 M accounts, θ=0.99, hot payees scattered:

| | hit rate | modelled AMAT |
|---|---|---|
| after workload (scattered) | 0.699 | **139.3 ns** |
| after one consolidation pass | 0.928 | **86.6 ns** |
| oracle ceiling (`--sorted` prefill) | 0.957 | 79.8 ns |

**1.61×, capturing 88.5% of the available gap** — `(139.3−86.6)/(139.3−79.8)`.
Reproduces at 500 K accounts with 3 rounds and db_hash on: 143.0 → 87.0 ns.

Cost: 289 loglets, 5.98 M items relocated in 7.0 s single-threaded ≈ 85 MB/s. The
paper's back-of-envelope assumes 150 MB/s for a 256 GB heap relocated hourly, so the
rate is the same order; consolidation is deliberately serial (see below).

Design notes:

- **No `ItemHeader` change.** Hotness lives in the `Loglet` as a cached word
  (`hot_len_`), refreshed by `measure_hotness()` rather than maintained
  incrementally — an item's temperature changes as it ages with no corresponding
  event, so a running counter would drift regardless. This sidesteps all three
  bit-stealing blockers below, and preserves the paper's "no memory space overhead"
  claim.
- **`mixedness()`** is `1 − |2·hot_frac − 1|`: 0 when a loglet is one temperature,
  1 at an even split. A loglet can be perfectly compacted by liveness and maximally
  mixed, which is exactly why `compact()`'s selection rule never picks it.
- **`compact_one` takes an optional `is_hot`**, choosing between two per-thread
  destinations. The relocation contract, `modify_locked` re-check, and reserve
  handling are all unchanged — only the choice of destination differs.
- **Consolidation runs serially**, not on the pool. It has no memory-pressure
  deadline behind it, and serial relocation keeps two per-thread destinations from
  multiplying across 128 workers and re-triggering BUG-2's reserve drain.

`ramdb_test` 20/20, `hogdb_test` 214 passed / 2 skipped.

### Sweeps — `figures/v1_consolidation.{pdf,png}`, `python3 plot_v1.py`

**What is measured, and what is not.** This host has one NUMA node and no slow
tier, so nothing here times memory. Measured: every item's real loglet
(`for_each_location`) joined against an independent access trace, giving the
**fast-tier hit rate** — the share of accesses landing in the hottest 20% of
loglets. AMAT is then one line, `hit*70 + (1-hit)*300`, an affine rescaling
carrying no information the hit rate does not. Report the hit rate; treat every
nanosecond figure as contingent on three assumed constants (`kFastNs`,
`kSlowNs`, `kFastTierFraction`).

**(a) Hit rate vs skew** (2 M accounts), `data/v1_skew.csv`:

| θ | scattered | consolidated | rank-sorted ceiling | share of gap |
|---|---|---|---|---|
| 0 | 0.546 | 0.555 | 0.546 | no gap exists |
| 0.3 | 0.547 | 0.585 | 0.652 | 37% |
| 0.6 | 0.553 | 0.691 | 0.783 | 60% |
| 0.8 | 0.589 | 0.794 | 0.880 | 70% |
| 0.9 | 0.635 | 0.850 | 0.925 | 74% |
| 0.99 | **0.698** | **0.896** | 0.957 | **76%** |

Arms correctly ordered everywhere, all converging at θ=0 where no stable hot set
exists and consolidation achieves nothing. Benefit grows monotonically with skew.

**Headline, assumption-free:** at θ=0.99 the hit rate rises 19.8 points, 0.698 →
0.896 — **2.91× fewer accesses falling to the slow tier**, whatever that tier is.

**(b) Cost/benefit** (θ=0.99), `data/v1_mixedness.csv`:

| victims | loglets relocated | bytes moved | hit rate |
|---|---|---|---|
| `mixedness ≥ 0.10` | 0 | 0 | 0.698 |
| `mixedness ≥ 0.005 … 0.04` | **107 (37%)** | **214 MiB** | **0.896** |
| `mixedness ≥ 0` | 289 (100%) | 578 MiB | 0.896 |

**Identical result for 37% of the work.** The per-loglet mixedness distribution is
bimodal: 182 loglets are essentially pure cold (< 0.005) and relocating them
achieves nothing; 107 hold the mixed data and carry the whole benefit. This is the
measurement behind §6's unevidenced claim that "very hot and very cold objects
would rarely be relocated", and it speaks to the cleaning-cost debate §6 concedes
is unresolved.

Denominator: 290 loglets hold data (580 MiB); the heap is 17.7 GiB / ~9062 loglets
because the bench provisions 8× headroom to keep the compactor idle. Quoting cost
against the heap would read 1.2% rather than 37% for reasons unrelated to the work
done. 289 not 290 because one is the active write head, which consolidation cannot
relocate out of.

**(c) Fast-tier capacity.** The third assumed constant, and the one that matters
most. `data/v1_capacity.csv` holds the access-concentration curve — cumulative
share of accesses against share of loglets, hottest first — so the hit rate at any
capacity is a point on it and one run characterises every tier size:

| fast-tier capacity | scattered | consolidated | gain |
|---|---|---|---|
| 5% | 0.326 | 0.800 | **+47.4 pp** |
| 10% | 0.473 | 0.832 | +35.9 pp |
| 20% (what is quoted) | 0.698 | 0.895 | +19.6 pp |
| 30% | 0.886 | 0.957 | +7.1 pp |
| ≥50% | 1.000 | 1.000 | **0** |

**20% is not a conservative choice — it is mid-slope.** The benefit is ~2.4× larger
at a 5% tier and vanishes entirely by 50%, where the hot set fits regardless of
layout so there is nothing to consolidate. Any claim of the form "consolidation is
worth X" is really "worth X at this tier size", and the tier size is the parameter
an operator actually picks.

**(d) Slow-tier latency.** The same measured hit rates give:

| assumed slow tier | modelled speedup |
|---|---|
| 150 ns | 1.20× |
| 300 ns (Optane, what is quoted) | **1.49×** |
| 500 ns | 1.74× |
| 1000 ns (RDMA-class) | 2.11× |

So "1.49×" means "1.49× if the slow tier is 300 ns". The curve asymptotes to the
2.91× miss-reduction as the slow tier gets slower.

**One more assumption, not swept:** the fast tier is chosen by an *oracle* that
takes the top-N loglets by true access count — the paper's method ("if a
memory-tiering system had an oracle that perfectly picked out the top 20% of
hottest pages"). Real tiering systems classify by page faults, ACCESSED bits or
PEBS sampling and pick worse than perfect. That handicaps the scattered baseline
less than reality would, so the measured improvement is conservative in this one
respect.

### Correction: earlier numbers were inflated by train-on-test

First reported 1.61× and 88% of the gap at θ=0.99, and a spurious 122.0 ns at θ=0.
Both came from building the hotness labels out of the same access counts the hit
rate was then scored against — the pass was rewarded for separating items by their
realized sample counts and then graded on those counts. At high skew that is nearly
benign (Zipf hotness is sample-stable); at θ=0 it manufactured a benefit out of
pure sampling noise.

With an independent scoring trace: **1.49× and 76% at θ=0.99**, and θ=0 correctly
flattens to 1.01×. The honest headline is 1.49×. The cost/benefit structure in (b)
is unchanged — only the AMAT it plateaus at moved, 86.6 → 93.8 ns.

The rank-sorted arm is now a genuine ceiling: it tracks the *distribution* rather
than one noisy sample, so it sits at or below the consolidated line everywhere. It
is still only informative where rank tracks popularity — at θ=0 rank carries no
information and it collapses onto scattered, which is why it is labelled
"rank-sorted prefill" and not "ceiling".

### B2 result — a practical classifier matches the oracle, once it sits above the cache

`HeatSketch` is a count-min sketch over item-key hashes: 4 rows × 2^w one-byte
counters. Off-header, so it avoids all three `ItemHeader` blockers, and **O(1) in
the item count** rather than O(n) — which is what preserves the paper's
no-per-object-overhead claim.

**Where it is fed is the whole result.** First attempt counted on ramdb's read
path and failed completely: recall 0.66 at a 4 MiB table but **0.0003 at 64 MiB**.
A larger table performing worse is impossible for count-min if the signal is real
— it only over-counts — so the 4 MiB figure was collision inflation pushing cold
items over the threshold, and its apparent 57% recovery of the oracle's benefit
was an artifact. *A too-small count-min sketch can look like a working classifier;
always check that a larger one does not do worse.*

The cause is [block_tx.cc:248](../../hogd/hogdb/block_tx.cc#L248): ramdb is read
only when an item is **absent from the block's lock cache**, so the 2nd through
Nth access to an item within a block never reaches the storage engine. ramdb sees
one read per item per block; access frequency within a block is invisible below
`BlockTX`. With a collision-free sketch, one block gave recall 0.0004 and four or
more gave recall 1.0 at base-rate precision — no information either way.

Counting instead in `BlockTX::read_nnint128_base`, above the lock cache, fixes it.
2 M accounts, θ=0.99, scattered start at hit 0.6983:

| sketch | table | precision | recall | hit after | space vs 580 MiB live |
|---|---|---|---|---|---|
| w=20 | 4 MiB | 0.400 | 1.000 | **0.9055** | 0.7% |
| w=22 | 16 MiB | 0.922 | 1.000 | 0.8984 | 2.8% |
| w=24 | 64 MiB | 0.999 | 1.000 | 0.8964 | 11% |
| oracle | — | 1.000 | 1.000 | 0.8964 | — |

**A real classifier recovers the oracle's entire benefit**, and the space cost is
constant in store size — the same 16 MiB is 2.8% here and 0.006% of a 256 GB heap.
Recall is 1.0 at every size because count-min never undercounts.

Note the 4 MiB sketch slightly *beats* the oracle (0.9055 vs 0.8964). That is real
now that the counting is correct: its collisions make it over-inclusive, and
grouping singly-accessed items with the hot ones concentrates accesses marginally
better than the oracle's hard cut at 2 accesses. So the "oracle" was never the
ceiling — it is optimal only for its own threshold. The rank-sorted prefill
(0.9574) remains the real ceiling.

**A trap this exposed twice:** the first attempt double-counted, because
`write_nnint128` calls `get_before_item` → the ramdb read path, so every setup
*write* registered as an access. That made singly-accessed payees cross the
threshold and the sketch appear to beat the oracle. Both the storage-path
instrumentation and the apparent win disappeared together once counting moved to
the single correct point. Count at one layer, and confirm which operations reach
it.

Still open: `HeatSketch::halve()` exists but is never called, so the sketch tracks
"was ever hot" rather than "is hot". Aging needs an epoch and a workload whose hot
set moves before it can be evaluated at all.

### P1.4 — consolidation is now bounded, and reaches the newest data

Two structural gaps closed, both prerequisites for putting consolidation on a
trigger. Prompted by the flexkvs comparison: its cleaner picks one victim
segment per tick and never takes the list away from anyone, and both properties
turned out to matter here.

**Bounded claiming.** `consolidate()` used to swap the whole of
`compactable_loglets_` out, measure, and put the non-victims back. For the
duration a concurrent `compact()` saw an empty list, reclaimed nothing, and
`background_worker` escalated. It now claims in batches of 32 and returns each
non-victim immediately, so the list is never empty, and takes a `max_victims`
bound so a pass can be sized rather than run to completion. The budget is fixed
at pass start because non-victims go back on the end of the list — without that
a pass keeps meeting the loglets it just returned.

**Write-head rollover.** A per-thread head reached the compactable set only when
it filled, so up to one loglet per writing thread of the *newest* items was
permanently invisible to consolidation. `Database::consolidate()` now calls
`ItemAllocator::roll_over_heads()` first, which hands every head over without
taking a replacement — no allocation, so it cannot block, and an idle thread is
left holding nothing. Safe because `consolidate()` already forbids overlapping
`put()`/`remove()`.

`DatabaseTest.ConsolidateWriteHeads` relocates **128 loglets** where the old code
found zero: exactly the one-per-core figure the earlier note predicted, now a
test rather than a caveat.

One wart the rollover exposed: an idle thread's head is *empty*, and an empty
loglet has `mixedness() == 0`, so at `-m 0` all 128 were selected and counted as
victims — 192 "relocated" loglets against 73 occupied. Loglets with no live bytes
are now skipped and left to `compact()`, which is the code that can actually
reclaim them.

### P1.2 — the threshold comes out of the sketch, not a flag

`HeatSketch::hot_threshold(f)` histograms the table's non-zero counters and
returns the count at which the hottest `f` of tracked keys are hot. The table is
its own count distribution — every key occupies one counter per row — so this is
exact for a collision-free sketch, and collisions bias it up, which under-selects
rather than over-selects. Exposed as `hogdb_bench -F/--hot-fraction`.

This is the same idea as flexkvs's `get_victim_threshold_smooth`, which samples
100 hash buckets every millisecond and takes an EWMA-smoothed 80th percentile.
Reading the sketch is strictly better: one pass, exact, deterministic, no RNG and
no smoothing constant, and the knob is a *fraction of the working set* rather
than a percentile of a sample.

500 K accounts, θ=0.99, 3 rounds, w=22 sketch, scattered start at hit 0.6824
(143.06 ns):

| `--hot-fraction` | derived threshold | hit after | AMAT |
|---|---|---|---|
| 0.02 | 12 | 0.8672 | 100.54 ns |
| 0.05 | 6 | 0.8934 | 94.53 ns |
| 0.10 | 6 | 0.8934 | 94.53 ns |
| **0.20** | **3** | **0.8970** | **93.69 ns** |
| 0.40 | 3 | 0.8970 | 93.69 ns |
| `--hot-hits 2` (fixed) | 2 | 0.8970 | 93.69 ns |

**The optimum is the fast tier's own size.** `kFastTierFraction` is 0.20 and the
hit rate peaks there; asking for a hot set much smaller than the tier (0.02)
costs 3 points because genuinely hot items are left classified cold. So the
threshold is not a free parameter to sweep — it is determined by the tier, and
the sketch can now derive the corresponding access count on its own. That closes
the "inherited from a default" hole; what it does *not* do is close the gap to
the 0.9574 rank-sorted ceiling, which is still unexplained.

Note the plateau above 0.20 and the tie with the hand-set `-k 2`. The count
distribution here is shallow — the thresholds the sweep actually landed on were
1, 2, 3, 6 and 12 — so the curve is coarse and 0.20 is where the peak *begins*
rather than a resolved optimum. A deeper access trace is needed before reading
anything into the shape between those points.

### P2.0 — the quiescence budget, measured

The question gating the trigger design (2.1): *how much consolidation fits in the
gap between ledger closes?* It needs two numbers, and only one of them is a
property of ramdb.

**First finding, before any measurement: there is no quiescent window, because
nothing declares one.** `Database::set_active()` exists and its comment
describes consolidation's precondition exactly — *"when inactive, items should
not be accessed; this allows the database to run background activities that
could disrupt performance and may not be thread safe"* — but the only caller in
the tree is `db_test.cc`. hogdb never calls it. So the signal a quiescence
trigger would hang off is present in the API and unwired, and wiring it at the
block boundary is a prerequisite, not an afterthought.

**Cost per loglet.** `hogdb_bench -V N` bounds the pass; 500 K accounts, θ=0.99,
`-F 0.20`. Baseline scattered hit rate 0.6824.

| `-V` | loglets | seconds | s/loglet | hit rate | AMAT |
|---|---|---|---|---|---|
| 1 | 1 | 0.0236 | 0.0236 | 0.6764 | 144.42 ns |
| 2 | 2 | 0.0466 | 0.0233 | 0.6997 | 139.08 ns |
| 5 | 5 | 0.1166 | 0.0233 | 0.7404 | 129.72 ns |
| 10 | 10 | 0.2262 | 0.0226 | 0.8187 | 111.71 ns |
| **20** | **20** | **0.4570** | **0.0229** | **0.9047** | **91.91 ns** |
| 30 | 30 | 0.6095 | 0.0203 | 0.9047 | 91.91 ns |
| 50 | 50 | 0.8968 | 0.0179 | 0.9047 | 91.92 ns |
| 0 (all) | 82 | 1.3537 | 0.0165 | 0.8971 | 93.67 ns |

Cost is linear with no meaningful fixed component: **~23 ms per loglet, i.e. 43
loglets/s ≈ 87 MB/s**, matching the 85 MB/s measured for the unbounded pass. The
per-loglet figure drops toward 17 ms for later victims because those are the
partly filled write heads, which hold less.

**The full benefit costs 24% of the loglets and 34% of the time.** 20 victims
reach 0.9047 and 30, 50 and 82 do not beat it — the whole-heap pass is in fact
marginally *worse* (0.8971), consistent with the earlier mixedness finding that
relocating pure-cold loglets achieves nothing, and suggesting that spreading hot
items over more destinations dilutes them slightly. Unexplained; the 20/30/50
plateau is stable enough to be real rather than noise.

Note `-V 1` lands at 0.6764, a hair *below* the 0.6824 baseline. One relocated
loglet is not worth doing, and may be marginally harmful.

**Block execution time**: ~0.11 s for a 500 K-payment block (`block,` line).

**What this means for the trigger.** One loglet costs 23 ms and ~20 are needed,
so a single quiescent window would have to be ~500 ms to converge in one shot —
implausible. But the layout is frozen (D2): this is a one-time fix, not
maintenance. Spreading it at **one or two loglets per ledger close, converging in
~10–20 closes**, costs 23–46 ms of idle time per block and is entirely
practical. That is the flexkvs "one victim per tick" shape, arrived at
independently from the cost side, and `max_victims` already implements it.

**Caveat on the window itself.** The benchmark runs blocks back-to-back, so it
cannot measure idle time — the gap is the close interval minus execution, and
the close interval is a deployment parameter the bench does not model. What is
measured here is the cost side; the window has to come from a real node.

**A bug this measurement found.** The first sweep showed a one-victim pass
costing 75 ms against a 16 ms marginal cost — a fixed overhead that should not
exist. Cause: `measure_hotness()` ran on every loglet in the 32-loglet claim
batch before the victim budget was checked, so a bounded pass paid to walk the
item headers of 32 loglets to relocate one. Checking the budget first cut a
one-victim pass 3.2× (75 → 24 ms) and the full pass 21% (1.71 → 1.35 s). A
bounded pass that scans unboundedly is not bounded; the bound only became real
once it was timed.

### P2.1/P2.2 — the trigger works, and the layout converges then stops

`Database::consolidate_incremental()` runs one bounded pass classified by the
sketch, called at a quiescent point. `hogdb_bench -A N` calls it at every block
boundary. 500 K accounts, θ=0.99, `-F 0.20 -A 2`, so 2 loglets per close:

| block | 0 | 3 | 5 | 7 | 9 | 11 | 13 | 14+ |
|---|---|---|---|---|---|---|---|---|
| loglets moved | 2 | 2 | 2 | 2 | 2 | 2 | 2 | **0** |
| hit rate | 0.6997 | 0.7824 | 0.8368 | 0.8873 | 0.9028 | 0.9026 | **0.9045** | 0.9045 |

**It converges in 13 closes and then selects nothing at all.** From block 14
onward every pass returns 0 and the hit rate holds at 0.9045 indefinitely — equal
to the best single bounded pass (0.9047 at `-V 20`) and better than the unbounded
one (0.8971). Cost: ~28 loglets relocated in total, ~60 ms per block boundary
against a ~90 ms block.

**That is the answer to 2.2, and it is the one §6 predicted.** The layout
converges rather than consolidation fighting new writes, and the trigger goes
quiet on its own — no stop condition had to be invented, because "nothing is
mixed any more" is one. It is also the first result here measured over time
instead of as a single before/after pair.

`min_mixedness` at 0.10 and 0.20 behave identically (converge by block 13, then
stop), so the gate is not sensitive in that range.

#### Two things had to be fixed first, and both were found by measuring

The trigger did not work when first wired. 40 loglets relocated across 20 closes
reached hit 0.7534, against 0.9047 for 20 loglets in a single pass — more work
for a worse layout.

**(a) The destinations were abandoned between passes.** `consolidate()` flushed
both relocation destinations to the compactor before returning — the BUG-3 fix,
which was necessary because they lived in the calling thread's `thread_local`s.
So every pass started a *fresh, nearly empty* hot loglet, and 20 passes spread the
hot items over 20 sparse loglets where one pass packed them into a few. The flush
that made small passes safe is what made them useless.

Fixed properly rather than worked around: `CompactDests` moved into the
`ItemAllocator` as a member. That is the correct lifetime — `Database` declares
`loglet_allocator_` before `item_allocator_`, so the destinations are released
while the loglet allocator is still alive, which is exactly what BUG-3 needed and
what a caller thread's `thread_local` could never give. `compact_one` now takes
which destination pair to use; null still means the calling thread's own, so the
compaction path is unchanged. `flush_consolidation_dests()` ends a run, and the
destructor calls it so a partly filled destination is never freed with live items
in it.

Worth **0.7624 → 0.7534**, i.e. almost nothing on its own. It was a real defect
and fixing it was necessary, but it was not the cause.

**(b) `min_mixedness = 0` makes a repeating trigger eat its own output.** A
loglet that consolidation has already made homogeneous has mixedness 0, and
`0 >= 0` is true, so it stays eligible forever. Each pass spent its two-loglet
budget relocating loglets that earlier passes had already sorted. A single pass
never notices because its output only returns to the candidate list after the
budget is spent.

| `min_mixedness` | hit rate after 20 closes |
|---|---|
| 0 | 0.7534 |
| 0.01 | 0.8179 |
| 0.05 | 0.8979 |
| **0.10** | **0.9045**, then terminates |

So the policy default is 0.1, not the 0 a one-shot pass uses. **The lesson is
that a threshold which is merely a cost/benefit knob for a single pass becomes
the termination condition for a repeating one** — the same parameter changes role
entirely once the mechanism runs more than once.

#### BUG-4 — the shipped default did not deliver the measured result

The convergence above was measured with `-m 0.10` passed explicitly. When 0.1 was
then made the policy *default*, `Database::enable_auto_consolidation` got the new
default but the `RAMDB` wrapper in `hogdb_dao.h` kept its own `= 0.0F` — and a
wrapper's default wins, because it forwards an explicit argument. So every caller
using the default silently got the broken configuration, and the trigger stopped
converging: 0.7557 at block 15 instead of 0.9045.

Found only because the aging experiment's control run disagreed with a result
measured an hour earlier. Nothing failed; the number was just quietly worse.

**This is BUG-2 again** — that bug was `reserved_count_` and `estimate_heap_size`
drifting apart, fixed by naming one constant both used. The same fix applies:
`kAutoConsolidateMinMixedness` now lives in `constants.h` and both the Database
method and the DAO wrapper default to it. A defaulted argument duplicated across
a forwarding layer is the same hazard as a duplicated constant, and it is less
visible.

**Process note:** re-run the control after changing a default, not just the
tests. The tests passed throughout — `ConsolidateIncremental` passes
`min_mixedness` explicitly, precisely so it can check the budget, so it could
never have caught this.

#### Why not `set_active()`, and what production still needs

`set_active(false)` looked like the natural hook and is not, for two reasons.
It calls `pool_.adjust_workers()`, so driving it per block would resize the
worker pool on every close. And it is a *promise* about a window, not an event:
notifying a background thread and letting it consolidate later would relocate
items at some moment when writes may have resumed, breaking the precondition. So
`consolidate_incremental()` is synchronous and runs on the committing thread,
which is the only place the promise is still true.

**Production is not wired, deliberately.** The hook belongs in
`HogDBSchedRpc::commit_block` after the block tx is shut down, and it must be
gated on `RPCContext::block_txes_` being empty — a viewing or sibling block still
holding a reference means the store is not quiescent and the precondition fails.
That is a change in the consensus RPC path and needs an owner's review rather
than a guess from here. What is proven is the mechanism, the policy, and the
convergence; what is missing is one call site and its emptiness check.

### P1.1 — without aging the trigger goes silent and the layout stays broken

The convergence result above was measured against a hot set that never moves,
which is exactly the case where "was ever hot" and "is hot" are the same thing.
`hogdb_bench -E K` re-draws the rank → payee permutation every K blocks, so the
hot accounts change identity while the skew stays identical; `-G K` halves the
sketch every K blocks. 500 K accounts, θ=0.99, `-A 2 -F 0.20`, 40 blocks, hot set
re-drawn at blocks 15 and 30.

| block | 13 | 14 | 15 | 18 | 19–29 | 30 | 32–39 |
|---|---|---|---|---|---|---|---|
| **no aging** | 0.9046 | 0.9046 | 0.6718 | 0.7055 | **0.7055, moved=0** | 0.6868 | **0.6868, moved=0** |
| **halve /5** | 0.9044 | 0.9044 | 0.6797 | 0.7076 | 0.73–0.74 | 0.6977 | rising to 0.8813 |
| **halve /2** | 0.9045 | 0.9045 | 0.6788 | 0.7183 | rising to **0.9044** | 0.6789 | rising to 0.8962 |

**Un-aged tracking does not degrade gracefully — it stops.** Four blocks after the
hot set moves, the trigger selects nothing (`moved=0`) and the layout sits at
0.7055 for eleven consecutive blocks. After the second move it ends at 0.6868,
which is within half a point of the 0.6824 scattered baseline: **the entire
benefit is gone and the system is reporting that there is nothing to do.**

The mechanism is that the sketch never forgets. It still says the *old* hot
accounts are hot, so the loglets consolidation built for them still look
homogeneous, no loglet crosses `min_mixedness`, and the pass terminates. The stop
condition that made the static result so clean — "nothing is mixed any more" — is
indistinguishable from "my classifier is looking at the wrong accounts". That is
the worst available failure mode: silent, and it looks exactly like success.

**Aging fixes it, and the epoch matters.** Halving every 2 blocks recovers the
full 0.9044 within 11 blocks of the move — the same order as the initial
convergence — and recovers again after the second move. Halving every 5 blocks
recovers slowly and had not finished within 9 blocks. So `halve()` stays; the
question was whether it was dead code, and it is not.

**The trade-off is that aging never lets the trigger go quiet.** Under `-G 2` the
pass relocates 2 loglets on every block indefinitely, including blocks 24–29 where
the hit rate is already ~0.90 and it is only holding position — ~60 ms of every
block, forever. Un-aged does no work and holds a bad layout; aged holds a good
layout and never stops paying. Neither is the right answer: the epoch wants to be
driven by *detected* drift rather than a fixed period, so a static hot set costs
nothing and a moving one is tracked. That is the obvious next design step and is
not implemented.

All three arms converge identically to 0.9045 by block 13 before the first move,
so aging costs nothing while the hot set is stable.

**Caveats.** Two moves observed, two epochs tested, one skew. A full re-shuffle is
also the most violent possible drift — every hot account goes cold at once —
where real traffic drifts partially. Both make this a demonstration that aging is
load-bearing, not a calibration of the epoch.

### P3a — the SSD tier ran and measured nothing, and the reason is structural

`run_ssd_tier.sh` backs the loglet heap with a file on NVMe and caps the process
with `systemd-run -p MemoryMax`, so resident pages are the fast tier and evicted
ones return from the device on a major fault. That is the only tiering measurement
available without CXL or NVM hardware, and there will not be any.

First run, 500 K accounts, θ=0.99, cap 1200M, `data/ssd_tier.csv`:

| arm | cap | major faults | hit rate |
|---|---|---|---|
| scattered | uncapped | 0 | 0.6824 |
| consolidated | uncapped | 0 | 0.8932 |
| scattered | 1200M | **0** | 0.6824 |
| consolidated | 1200M | **0** | 0.8932 |

**Zero major faults in every arm: the cap never bound.** 500 K accounts occupy 73
loglets, i.e. 146 MB of live data against a 1200 MB cap — the working set is
eight times smaller than the fast tier, so nothing is ever evicted and there is no
slow tier to be on the wrong side of. The 3 GB heap *file* is misleading; it is
provisioned space, almost all untouched.

**Why raising the account count does not fix it by itself.** The bench holds
per-account state that is not reclaimable: one `Payment` in `ops_` for every
account, and two `unordered_map<std::string, uint32_t>` hit tables keyed by the
32-byte ramdb key. That grows at roughly the same rate as the live heap, so
scaling up to exceed the cap raises the anonymous floor too, and the cap starts
OOM-killing the process instead of paging the heap — which is the failure the
script's own `CAP` comment warns about.

**What it needs.** Make the heap dominate the footprint at measurement time:

- Replace the two string-keyed hit maps with vectors indexed by payee id. They are
  the largest per-account cost and the id is already dense.
- Free `ops_` and the hit tables before `replay_reads()`. The fault measurement
  only needs `payee_slot_` (4 bytes per account) and the Zipf generator, so the
  workload state can be released before the arm that is being measured.
- Then scale the account count until live heap is several times the cap.

Until that lands, every tiering number in this document remains modelled. The
layout results do not depend on it — they are hit rates from allocator
introspection — but the fault count was supposed to be the one unmodelled figure,
and it is not yet a figure at all.

### P3b — an emulated fast tier, and the first unmodelled number

Yes, the fast tier can be shrunk, but not with a resident-set cap. `-T` evicts
every loglet outside the fast tier from both the mapping *and* the page cache, so
a later access to one is a real read from the SSD. `-P` sets the tier's share of
occupied loglets, so its size is exact and sweepable rather than inferred.

500 K accounts, θ=0.99, one eviction before the replay, `data/tier_emulation.csv`:

| tier | scattered faults | consolidated faults | ratio | modelled hit (scat → cons) |
|---|---|---|---|---|
| 5% | 2944 | 1520 | 1.94× | 0.277 → 0.791 |
| 10% | 2797 | 1430 | 1.96× | 0.454 → 0.829 |
| 20% | 2538 | 1265 | **2.01×** | 0.682 → 0.893 |
| 30% | 2312 | 1114 | 2.08× | 0.879 → 0.957 |
| 50% | 1805 | 788 | 2.29× | 1.000 → 1.000 |
| 100% | **0** | **0** | — | 1.000 → 1.000 |

**The consolidated layout reads about half as much from the device, at every tier
size.** 1.94–2.29×, stable across a 10× range of tier sizes. This is the first
figure in this document that assumes none of `kFastNs`, `kSlowNs` or
`kFastTierFraction` — it is counted, not modelled.

**Two checks that the instrument is sound.** Keeping every occupied loglet
(`-P 1.0`) gives exactly zero major faults in both arms, so faults come only from
evicted loglets and there is no page-cache or disk-buffer confound. And fault
counts fall monotonically as the tier grows, in both arms.

#### What does not reconcile, and should not be glossed

At a 50% tier the modelled hit rate is 1.0000 for both arms — no misses at all —
yet 1805 and 788 faults are measured. A hit rate of 1.0000 to four places allows
~25 missed accesses out of 500 K; the measurement is ~70× that. The ratio also
*grows* with tier size, where the model says the benefit should vanish by 50%.

So the fault count and the modelled hit rate are measuring subtly different
things, and the discrepancy is unexplained. **Do not present the two as
confirming each other.** The ~2× fault ratio stands on its own as a measurement;
the claim "measured AMAT tracks the model", which was Phase 3.3's purpose, is not
supported by this and is still open. Next step is to instrument which loglets the
replay actually faults on and compare that set against the ones the join counts
as cold.

#### Two dead ends worth recording

**A resident-set cap cannot isolate the read phase.** `run_ssd_tier.sh` caps the
whole process for its whole life, but the write phase needs far more resident heap
than the replay being measured. Measured: 404 MiB anonymous (unreclaimable with
swap off) against 367 MiB of heap mapping. The sweep goes from no faults at 700M
straight to OOM kills at 600M — there is no cap in between that both survives
setup and squeezes the replay. Explicit eviction sidesteps this entirely and makes
the tier size exact rather than a subtraction.

**`MADV_DONTNEED` alone does not reach the device.** It drops the mapping and
leaves the page in page cache, so the next access is a *minor* fault: the first
attempt measured 748 K minor faults and zero major ones. The backing-file
descriptor is now retained so `posix_fadvise(POSIX_FADV_DONTNEED)` can evict the
cache entry too. Worth remembering — the run looked like it was working, and the
arms even differed in the right direction (748 K → 686 K), while no read was
reaching the SSD at all.

**Re-evicting during the replay does not work either.** It was meant to make the
count per access rather than per distinct cold page. At an interval of 1000
accesses it produced 1.03 major faults per access in *both* arms — exactly one per
access, i.e. each sweep was dropping the hot set too, since msync + fadvise over
the ~3 GB cold region hundreds of times puts the whole file under writeback and
reclaim pressure. Default is now a single eviction, and the measurement is
therefore first-touch: distinct cold pages, which under-counts repeat misses and
is conservative.

### BUG-3 — `consolidate()` freed loglets through a dangling allocator

Introduced by V1 and **shipped in 29290751**; found only when a later change made
me check the test process's exit code rather than its output.

All 22 tests pass and then the process aborts in teardown:

    Fatal glibc error: pthread_mutex_lock.c:426: assertion failed: e != ESRCH || !robust

`compact_one` keeps its relocation destinations in `thread_local`s that
deliberately outlive one call. That is safe for `compact()`, which runs on the
WorkerPool: `Database` declares `pool_` before `loglet_allocator_`, so workers are
joined — releasing their destinations — while the allocator is still alive.
`consolidate()` runs **serially on the caller's thread**, whose `thread_local`s are
destroyed at thread exit, long after the allocator. Freeing a loglet through the
dangling allocator aborts inside its mutex. Choosing serial execution is what
introduced it.

Fixed by hoisting the destinations into an accessor and having `consolidate()` hand
both back via `give_to_compactor` before returning. That also stops a partly filled
destination being stranded where nothing can reclaim it. Verified over three runs:
exit 0, no abort, 22/22.

**Process note:** earlier reports of "ramdb_test 22/22" were checking the summary
line, not the exit status, so this was missed for several commits. Check exit codes.

### Still to do here

- ~~A dedicated integrity test for `consolidate()`~~ — **done**:
  `DatabaseTest.Consolidate` and `.ConsolidateAllCold` in `db_test.cc` check the
  whole DB against the oracle after a pass, after a second no-op pass, and after
  further removes/inserts through the repointed index. Writing it immediately found
  that `consolidate()` sees **nothing** unless write heads have rolled over: a
  per-thread head reaches the compactable set only when it fills, so `insert_n()`
  spread across 128 cores leaves every head partly full and the candidate set
  empty. Tests insert serially now. The underlying limitation is real and
  unaddressed — **up to one partly-filled loglet per core (256 MB on this box) of
  the newest data is invisible to consolidation.**
- `fast_hot` barely moves (0.204 → 0.245) because payer balances and replay entries
  dominate the item population and dilute it. Hit rate is the metric that drives
  AMAT; worth stating plainly rather than reporting utilization as the headline.
- Real classifiers (B2) measured against this oracle.
- Cost at scale, parallel consolidation, and a trigger policy (the paper's third
  §5 component) instead of an explicit call.

### Earlier consequence for D2 (superseded by the above)

Groundhog p2p payments are the correlated case: an account being transacted is both read
and written. If the deployed workload sits near ρ=1, ObjecTier's headroom there is closer
to 1.10× than 2.03×, and the honest framing for the "real application" section is that
log-structured ledger stores largely self-consolidate. Worth measuring the actual
correlation in ledger traffic before claiming otherwise.

### Next: sweep the correlation

Two extremes are a result; the curve is the figure. Interpolate with probability ρ that a
given overwrite targets the read-hot rank space instead of the independent one, and plot
AMAT against ρ. That produces a figure the paper has no analogue for and locates any real
workload on it. Cheap — one knob in `churn_items`.

---

## X3 result — §4.3's prediction confirmed, but we are on the plateau

25 M items (fixed), θ=0.99, 32 threads, 3 accesses/item, `value_size` swept. Item count
is held constant so the rank population, skew, and drawn access sequence are byte
identical at every point; only items-per-loglet varies. Data in `data/x3_size_sweep.csv`,
per-point CDFs in `data/x3_cdf_*.csv`, driver `run_x3.sh`. Whole sweep ran in ~7 min.

| value_size | items/loglet | scattered AMAT | consolidated AMAT | ratio |
|---|---|---|---|---|
| 16 B | 43,690 | 198.0 ns | 92.3 ns | **2.145×** |
| 32 B | 32,768 | 195.3 ns | 92.4 ns | 2.114× |
| 64 B | 21,845 | 191.5 ns | 92.4 ns | 2.073× |
| 128 B | 13,107 | 186.6 ns | 92.4 ns | 2.019× |
| 256 B | 7,281 | 180.2 ns | 92.4 ns | 1.951× |
| 512 B | 3,855 | 173.7 ns | 92.4 ns | **1.880×** |

Monotonic: **smaller items ⇒ larger consolidation benefit**, exactly as §4.3 predicts and
never measured. The paper can now cite a real store for it.

Figure in `figures/x3_item_size.{pdf,png}`, regenerated by `python3 plot_x3.py`. Drawn on
the same form as the paper's Fig 3 (AMAT vs a swept parameter, two layouts, tier floors)
so the two read as siblings — Fig 3 sweeps skew, this sweeps granularity.

Draft caption: *Modelled AMAT against item size, 25 M items at θ=0.99. Consolidated is
flat: popularity sorting packs the hot set contiguously whatever the item size. The
scattered baseline improves as items get larger, because fewer items per loglet means
random placement homogenizes less. The gap therefore widens toward small items, as
Section 4.3 predicts.*

**The mechanism is on the scattered side, and consolidated is flat by construction.**
Consolidated AMAT is constant at 92.4 ns (hit rate 0.9025–0.9029) — perfect popularity
sorting packs the hot set contiguously whatever the item size, so the hit rate is set by
the access distribution and the 20% capacity, not by granularity. What moves is the
scattered baseline: with fewer items per loglet the per-loglet access-count sum
concentrates less (Zipf's heavy tail dominates a small bin), so random placement produces
more spread and the top 20% catches more traffic by luck — scattered hit rate climbs
0.4435 → 0.5489. More items per unit ⇒ random placement homogenizes more thoroughly ⇒
consolidation is worth more. That *is* the paper's mechanism, seen from the baseline side.

`mean_hot` is 0.400 at every point, as it must be — it is the fraction of items touched
by the access distribution, which item size cannot change. Another self-consistency
check worth keeping.

### The important caveat: this range is saturated

At 2 MB loglets ramdb packs 3,855–43,690 items per tiering unit. The paper's FlexKVS
experiment had **four** 1 KB objects per 4 KB page. We are three to four orders of
magnitude deeper into the many-objects-per-unit regime, which is why the curve is a
shallow plateau (2.15 → 1.88 across an 11× density change) rather than the steep effect
the paper's numbers imply. Reaching their granularity means shrinking the *tiering unit*,
not growing the item — i.e. `kLogletRegionSize`, which is **A2**. X3 establishes the
direction and that ramdb sits near the ceiling of it; A2 is what would traverse the
interesting part of the curve.

### Caveat to state precisely in any writeup

This run is prefill-only: no deletes, no churn, so **no compaction ran**. The 0.993 live
utilization is dense log append, not the compactor maintaining it. The conclusion
(liveness ≠ hotness) is unaffected, but the sentence "ramdb compacts to high liveness and
low hotness" is not yet demonstrated — it needs a churn workload that actually triggers
`compact()`. Add that before claiming it.

---

## Revised spine

1. **X1** — the scattered-hot-data problem is real in a production log-structured store
2. **X2** — the static ceiling available on this store
3. **X4** — where real ledger skew falls on the Fig 3 curve
4. **B1 + V1** — first working ObjecTier: all three §5 components
5. **X5 + C1** — measured cleaner cost, real fast tier

B2 (classifier study) drops from headline to sensitivity analysis: with `lsn` free and V1
handling victim selection, the implementation is the novel part, not the classifier.

---

## Design decisions

**Classifier = `lsn`, not a new counter.** Paper §4.1's core principle is to tap
information the application *already collects* (memcached's eviction queues). ramdb's
equivalent is `ItemHeader::lsn` — already maintained, already survives `clone()`, zero
new bytes. `is_hot(h) := (cur_lsn - h->lsn) < W`. This is the faithful instantiation, not
a workaround.

**Victim selection by mixedness, not liveness.** Paper §4.2 selects regions whose hot
fraction is "neither near 0 nor 1" via "one additional word of metadata (a counter)".
ramdb's `compact()` selects by `ll->utilization() <= thresh` — pure liveness. V1 adds a
per-`Loglet` hot counter next to `free_len_`. Lives in the `Loglet` control structure,
**not** `ItemHeader` — zero item-density cost, serves the abstract's "without adding any
memory space overhead" claim.

**`compact_one` already *is* the paper's Object Relocator.** §5 specifies
`bool (*relocate)(void*, char* dst, size_t bytes_available)` that may refuse on
insufficient space, whereupon ObjecTier allocates a fresh region and retries. That is
exactly `clone()` → `nullptr` → `give_to_compactor` → `alloc_loglet` → retry in
[item_allocator.cc:233-240](../../hogd/ramdb/item_allocator.cc#L233-L240).

**Missing §5 component: Relocation Policy Manager.** ramdb realizes this least well.
Compaction is demand-driven only (`notify_low_mem` → `run_compact_`), so temperature
routing only acts under memory pressure. Trigger policy is an independent variable, not
a caveat. Candidate: segregate on `set_active(false)`, i.e. between ledger closes.

---

## BUG-1 — `LogletAllocator::alloc` segfaults when the compaction reserve drains

Pre-existing ramdb bug, hit the first time any experiment here made the compactor run.
**Reproduces deterministically** (2/2 runs, SIGSEGV, min free 10% = `kLowMemoryRatio`):

    ramdb_bench <db> churn_consolidated 10000000 64 0.99 16 8 1.5

Backtrace:

    LogletAllocator::alloc(LogletMode, bool)          <-- SIGSEGV
    ItemAllocator::compact_one(...)::lambda           <-- alloc_loglet, use_reserved=true
    ScalableMap::modify_locked(...)
    ItemAllocator::compact_one(UniqueLoglet&&)
    WorkerPool worker thread

Cause — [loglet.cc:210-213](../../hogd/ramdb/loglet.cc#L210-L213):

```cpp
  // allocate a free loglet.
  assert(!free_loglets_.empty());     // compiled out in Release (NDEBUG)
  Loglet *ll = free_loglets_.back();  // UB on an empty vector
  free_loglets_.pop_back();
```

With `use_reserved=true` the `alloc_cv_.wait` at :201-208 is skipped entirely, so nothing
guarantees the free list is non-empty. The header's "can never fail or block because it
draws from a pool of loglets reserved for compaction" is enforced *only* by that assert,
which Release deletes. Exhaust the reserve and it segfaults instead of failing.

Why the reserve drains: `reserved_count_` is `available_parallelism()` (one loglet per
core, set in the `Database` ctor), but the compaction `WorkerPool` runs that many
`compact_one` calls at once and each one draws a *fresh* reserved loglet every time its
destination fills — `give_to_compactor` moves the full loglet to `compactable_loglets_`
rather than freeing it. Source loglets are not returned until each `compact_one`
finishes. So a large pass consumes reserved loglets far faster than it returns them.

Not caused by anything in this branch: the crashing path is `alloc` → `compact_one`,
neither of which is modified here. The only library edits are `loglet_index()` (a
math-identical extraction from `get_loglet`, not on this path), `for_each_location`
(not called during churn), and two atomic increments in `background_worker` *after*
`compact()` returns.

**This is the reserved-pool hazard flagged in the first design review, now demonstrated.**
It matters directly for B1: two per-thread destinations instead of one doubles the draw
rate on the same reserve, so B1 would make this strictly easier to hit.

### Fix applied — option (a), fail closed

Three coordinated changes:

1. `LogletAllocator::alloc` returns `nullptr` on an empty free list instead of reading
   `back()` off an empty vector. This is the contract `make_unique_loglet` already
   documents ("you must check the return value"); `alloc` simply wasn't honoring it.
2. `compact_one` checks both allocation sites, sets `exhausted`, and stops. Because live
   items remain in the source, it hands the source back via `give_to_compactor` rather
   than freeing it — freeing a partially compacted loglet would lose live data. The
   deferred loglet is picked up by a later pass.
3. `CompactionStats::loglets_freed` is now `ptrdiff_t`, and `compact()` clamps at zero.
   It is a *net* count (sources freed minus destinations taken), so a pass that takes
   more than it frees went negative and wrapped — reading as an enormous "reclaimed"
   figure that would convince `background_worker` memory had been recovered. Change (2)
   makes that case reachable far more often, so it had to be fixed alongside.

Verified: the repro above now exits 0 on both runs with all 10 M items intact and
compaction doing real work (2 passes / 688 MB, 1 pass / 252 MB). `ramdb_test` 20/20.
`db_crash_test` ran 10 min / 1459 recovery-snapshot cycles with no corruption, panic, or
assert — it is an open-ended loop, so it was stopped rather than run to completion.

## BUG-2 — the compaction reserve is structurally too small

Not a crash any more, but the same repro logs **930 and 561 deferrals** per run: most
loglets are handed back partially compacted. Compaction still makes progress, just far
less per pass than intended.

`reserved_count_` is `available_parallelism()` — one loglet per core — but the compaction
`WorkerPool` runs that many `compact_one` calls concurrently, each needing at least one
destination, and each drawing a *fresh* one every time its destination fills. Meanwhile
sources are only returned as individual `compact_one` calls complete. Compaction fires
exactly when free loglets are scarce, so the pool starts already starved: all workers
grab a destination, the reserve hits zero, and everything after that defers.

Note `estimate_heap_size` already provisions `available_parallelism() * 3` loglets for
compaction overhead, so the space exists — only `reserved_count_` is set to 1×.

### Root cause: the reserve and the heap budget had drifted apart

`Database::estimate_heap_size` provisions *"two loglets per compactor thread and one
loglet per worker thread"* — `available_parallelism() * 3`. But the `Database` ctor
constructed the allocator with a reserve of `available_parallelism()`, i.e. **one** per
compactor thread. The heap was sized for a reserve the allocator never kept.

One per thread is exactly one short of workable: every worker takes a destination at
pass start (free is already scarce, which is why compaction fired), the reserve hits
zero, and the first worker to fill a destination has nowhere to roll over to.

### Fix applied

- New `kCompactionLogletsPerThread = 2` in `constants.h`, used by **both** the ctor's
  reserve and `estimate_heap_size`, so the two can no longer drift.
- Defensive clamp in the `LogletAllocator` ctor: the reserve cannot exceed a quarter of
  the region's loglets. Ordinary allocation waits for free to *exceed* the reserve, so a
  reserve at or above the total would make that wait unsatisfiable and turn every
  allocation into an OOM. Callers using `estimate_heap_size` are well clear; the clamp
  covers heap sizes chosen directly.

Measured on the same repro (10 M items, heap 1.5×, 8 rounds):

| | deferrals | reclaimed |
|---|---|---|
| before | 930 / 561 | 688 MB over 2 passes, 252 MB over 1 |
| after | **167 / 156** | **522 MB in 1 pass, 533 MB in 1** |

~5.5× fewer deferrals and ~1.7× more reclaimed per pass. `ramdb_test` 20/20;
`db_crash_test` 8 min with no corruption, panic, or assert.

### Residual, not pursued

~20% of loglets still defer and free loglets still touch 0%. Compaction submits one task
per claimed loglet and the pool runs `available_parallelism()` of them at once, with no
regard for how much reserve exists — so a large enough pass will always outrun any fixed
reserve. Eliminating the tail means bounding in-flight compactions by available reserve,
which is a design change rather than a bug fix. Raising the constant to 3 would just be
picking a bigger number; 2 is defensible because it is what the heap budget already
documents. The remaining deferral is graceful and costs only deferred work.

---

## Blockers on the CODE_SKETCH bit-stealing design

Do **not** implement heat bits in the `storage` byte without addressing these. All three
are avoided entirely by the `lsn` classifier + per-`Loglet` counter design above.

1. **Breaks recovery.** `ScopedDiskWrite::modify` → `ll->clone(src)`
   ([disk_writer.cc:302](../../hogd/ramdb/disk_writer.cc#L302)) copies the storage byte
   verbatim to disk; `SafeItemHeader` rejects `storage > kCommit`
   ([disk_reader.cc:141](../../hogd/ramdb/disk_reader.cc#L141)). Any heat ≥ 1 makes every
   snapshot record for that item unreadable. Reached via `mutation_cb_`
   ([scalable_map.cc:209](../../hogd/ramdb/scalable_map.cc#L209)) and
   `add_item_to_snapshot` ([db.cc:336](../../hogd/ramdb/db.cc#L336)).
2. **Data race.** The compactor probes `storage` *without* the bucket lock via
   `atomic_ref` ([item_allocator.cc:209](../../hogd/ramdb/item_allocator.cc#L209)). A
   plain heat RMW under the bucket lock races it — exactly the hazard the comment above
   that line already cites ([atomics.ref.generic]/3).
3. **Breaks existing comparisons.** `Loglet::free`'s assert
   ([loglet.h:319](../../hogd/ramdb/loglet.h#L319)), and
   [scalable_map.cc:269](../../hogd/ramdb/scalable_map.cc#L269) / :276.

Also: there is no `static_assert(sizeof(ItemHeader)==24)` today; the layout is pinned
from the recovery side by `SafeItemHeader`'s size and offset asserts
([disk_reader.cc:158-165](../../hogd/ramdb/disk_reader.cc#L158-L165)).

---

## Measurement traps

- **Bucket-lock contention masks AMAT.** Skewed reads concentrate on few of the
  `kBaseLockOrder = 9` (512×cores) mutexes — contention rises exactly where the memory
  win is being measured. Separate lock wait from memory stall, or pin a high lock order
  for tiering runs and report it.
- **Reserved-pool drain.** Two per-thread destinations doubles reserved-loglet
  consumption, and reserved loglets are the low-memory valve — under the pressure that
  triggers compaction. Watch for unexplained escalations to `compact(1.0F)`.
- **D1 may measure nothing.** Incremental snapshots already filter *temporally* by `lsn`,
  so cold items are already skipped. Measure **distinct loglets touched** and I/O
  sequentiality, not `snapshot_bytes_written()` alone.
- **Fig 5's 60% carries zero cleaner cost** by construction. Any real end-to-end number
  should come in below it even when everything works. Say so up front.
- **`thread_local` destinations outlive their allocator.** Already true for `dst`;
  `Dests{hot, cold}` widens it. `ramdb_test` / `db_crash_test` build several `Database`
  objects per process.

---

## Reference numbers from the paper

- Fig 2 setup: 256 GB array, 2 MB huge pages, 128 B objects, Zipf θ=0.99, 5e9 accesses.
- Fig 2a metric: **binary** — fraction of bytes in a page touched *at least once*. Not
  frequency-weighted. Fast tier = top 20% of pages (50 GB DRAM vs 200 GB Optane).
- Scattered: hot pages ~50% cold. Consolidated: top 20% at 70–100%.
- AMAT 167 ns → 87 ns (DRAM floor 70 ns). 4 KB pages: 167 → 114 scattered, 88 consolidated.
- Fig 3: θ=0.5 still gives 1.3× slowdown; θ=0 makes consolidation pointless.
- Fig 5: FlexKVS, 4 threads, 8 GB DRAM + 8 GB Optane, 16 GB working set, 15 M × 1 KB
  pairs, 25% hot keys, 4 GB contiguous hot set. 60% throughput gain, 8% off all-DRAM.
- Cleaner cost: 256 GB heap, one relocation/object/hour ≈ 150 MB/s ≈ 0.06% of bandwidth.
  Back-of-envelope, never measured.

**ramdb's structural advantage over the paper's own setup:** ledger items are tens of
bytes in 2 MB loglets — ~40,000 items per tiering unit, versus 4 per page in the FlexKVS
experiment. §4.3 predicts a larger effect at smaller object sizes and never tests it;
`value_size` is already a bench parameter, so X3 tests it nearly for free.

---

## Harness notes

- Bench target is `ramdb_bench` (behind `BUILD_BENCHMARKS`), source `db_bench.cc`.
- Tests: `ramdb_test`, `db_crash_test`.
- `ItemAllocator::take_loglets()` is public and documented for snapshot quiesce — this is
  the X1 inspection hook. Iterate with the existing `for (ItemHeader &h : *ll)`.
- Loglet identity is pointer arithmetic: `(addr - base_addr_) / kLogletRegionSize`.
- `Database::get_each_parallel` gives key/type/value but **not** loglet identity — it
  cannot produce the X1 CDF on its own.
- AMAT: `perf mem` / PEBS. TLB: `perf stat -e dTLB-load-misses,dTLB-loads`.
- **Huge pages are not reserved on this host** (`HugePages_Total: 0`), so `Loglet` logs
  `"Couldn't mmap huge pages"` and silently falls back to 4 KB pages. The paper's setup
  is explicitly 2 MB huge pages ("commonly done to avoid TLB misses in memory tiering"),
  and A2's whole premise is the 2 MB-vs-4 KB tension — so every TLB and AMAT number is
  invalid until this is fixed. Needs root: `sysctl -w vm.nr_hugepages=N`.
- Host has 128 cores and 755 GB RAM; `/tmp` is a 378 GB tmpfs, so WAL traffic there costs
  RAM but avoids the slow NFS home.
- Pilot sweep script: `scratchpad/skew_sweep.sh` (not committed).
