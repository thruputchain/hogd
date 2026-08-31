# RV32 Gas Calibration

A repeatable measurement of what the production RV32 engine actually costs per
instruction, so `kInstructionGasSchedule` in `program.h` can be set from data
instead of guessed.

The engine measured is `ThreadedMachine` with tracing off, built optimized and
with LTO -- the same lowering, handlers, and codegen the node runs. Nothing
else is measured: not `Machine`, not the spec model, not the syscall bridge.

## Run it

```sh
B=build-release-ccache-tmp   # your Release build tree
scripts/rv32_gas_calibrate.sh $B --out /tmp/gas.json      # full run, a few minutes
scripts/rv32_gas_calibrate.sh $B -- --quick               # smoke run, ~40s
scripts/rv32_gas_calibrate.sh $B -- --op=DIV --repeats=25 # one opcode, hard
```

The build directory is required. It is not searched for and there is no
default: which tree the binary comes from decides what is measured, and a
benchmark that picks that for you can measure something other than what you
asked for without ever saying so. Name an existing Release tree and the script
builds only the `rv32_gas_calibrate` target into it -- a handful of files. Name
a path that does not exist yet and it configures a Release build there. Name an
existing non-Release tree and it refuses rather than reconfiguring something you
use for other things.

**The build configuration is part of the measurement.** What is being measured
is the compiled interpreter, and Release is not a casual optimized build: it is
`-O3` rather than `-O2`, plus `-march`, plus libc++ under clang, plus LTO across
the interpreter's translation units. An ad-hoc build produces numbers that look
entirely reasonable and describe a different interpreter, which is the worst
kind of wrong. So CMake stamps the build type and the exact flags into the
binary, every run records them, and `rv32_gas_calibrate` refuses to measure
anything but a Release build. (`--allow-nonproduction` overrides it, for working
on the harness itself and nothing else.)

Both targets build under CMake directly: `rv32_gas_calibrate` and
`rv32_gas_calibration_test`. Only the RV32 ISA core is linked -- no OpenSSL, no
protocol, no XDR -- so the target's own build stays small even though it lives
in the full tree.

Read the output with:

```sh
scripts/rv32_gas_report.py /tmp/gas.json              # per-opcode costs
scripts/rv32_gas_report.py /tmp/gas.json --schedule   # proposed integer costs
scripts/rv32_gas_report.py /tmp/gas.json --workloads  # pricing quality
scripts/rv32_gas_report.py /tmp/gas.json --predict    # do the costs add up?
scripts/rv32_gas_report.py before.json after.json     # what moved
```

## Why not fit a cost table to a corpus of programs

The obvious approach -- run a set of benchmark programs, count opcodes, regress
time on the counts -- does not work, and it fails in a way that produces
confident numbers rather than an error.

**The design matrix is collinear.** Real programs use opcodes in stable
proportions: loads come with address arithmetic, branches come with compares,
almost everything comes with `addi`. Columns of the count matrix are then
near-linearly-dependent, `X'X` is near-singular, and the coefficients are free
to trade cost between correlated opcodes without changing the residual. The fit
looks good and the per-opcode numbers are arbitrary -- often negative. Adding
more benchmark programs does not help if they are all *realistic*, because
realism is exactly what makes the columns move together.

**Cost is not additive in opcode counts.** An interpreter's per-instruction
cost depends on its neighbours. This engine makes that literal: the lowering
fuses nine instruction patterns, so `addi;addi` is one dispatch and `add;add`
is two. It also charges gas per straight-line run, so a branch costs an extra
`enter_run` that belongs to no single opcode. And the same opcode costs
different amounts depending on whether its result is consumed immediately (see
below). A model of the form `time = sum(count_op * cost_op)` has no room to say
any of that, so the regression distributes the discrepancy across whatever
opcodes happen to correlate with it.

**The response contains fixed overhead.** Setup, teardown, and loop control are
in the measured time but belong to no opcode, so the fit attributes them to
whatever opcodes appear in every program.

## What this harness does instead

**Measure a level, not a difference.** For each opcode and context, build one
kernel whose body is *entirely* that opcode, time it, and divide by the
instruction count. That is the per-instruction cost of a program made of that
instruction, measured absolutely.

This is the part that took two attempts. The harness originally measured the
*marginal* cost: a family of kernels identical except for how many body slots
held the target instead of a filler, with time fitted against target count so
the slope was the target's cost minus the filler's. That design is sound in
principle and unusable in practice -- a contrast between two large timings
absorbs whatever per-kernel layout bias differs between them, and once that
bias rivals the effect the slope goes negative. Roughly a third of sweeps
priced below zero, on two different microarchitectures. An instruction cannot
cost less than nothing, and averaging does not fix a sign.

A saturated body has nothing to subtract. There is no filler in it, so the
answer does not depend on what the filler costs; there is no fit, so there is
no bias to absorb; and there is no difference, so the result cannot be
negative. It is also the question worst-case pricing asks directly: if a block
is filled with this instruction, what does each one cost?

The sweep is still built and still reported, as a diagnostic column and as the
fallback for the few opcodes that cannot be saturated.

**Suppress fusion, and check that you did.** A saturated body puts copies of
the target next to each other, which is exactly the arrangement a fusion rule
matches. So the harness does not try to detect fusion and discard the result --
it lowers those bodies with fusion off (`PreInstantiation::create`'s `fuse`
flag, defaulted true, untouched by production) and measures the unfused cost.
That is the cost the schedule charges anyway: fusion is a saving the engine may
or may not get, and worst-case pricing cannot assume it.

Detect-after-the-fact was the earlier approach and it has a blind spot worth
recording. The audit compares each slot against the same word lowered *in
isolation*, but the isolated probe is `filler, target, filler` and the filler is
an `add` -- so for target `add` the probe fuses exactly as the body does, both
sides match, and the audit reports clean. The cell every ratio used to be
divided by was the one cell the audit structurally could not check. With the
flag, all 100 single-target cells saturate rather than 98; `ADDI` is measurable
for the first time, having declined only because `addi;addi` is a rule.

Pair rows are the opposite: `auipc;jalr` exists to measure the fused form, so
those keep fusion on and their audit requires the rule to fire.

In the contrast sweep, targets are instead placed non-adjacently and separated
by an R-type `add` filler on private registers, which no fusion rule matches in
either position. That is a claim about the lowering, so
`lowering_audit.h` verifies it: it lowers each kernel exactly as production
would, and compares every body slot's baked handler against the handler the
same encoded word gets when lowered in isolation. Any fusion or run split shows
up as a mismatch and the sweep is rejected rather than reported. A test asserts
the audit fires on a deliberately fusible body, so a clean audit is evidence
rather than a tautology.

**Measure contexts, not a number.** An opcode does not have one cost. It has a
range, and which end applies is the guest author's choice. So each opcode is
measured in more than one arrangement -- results consumed immediately versus
not, branches taken versus not, memory hot versus spread versus pointer-chased,
divides by magnitude and dependent versus independent -- and the report takes
the worst. Pricing the average is pricing the case an adversary will not pick.

This axis is not decoration. On the development host, independent divides
measured 1.26 ns and dependent ones 5.31 ns; independent L1-resident loads
measured 0.98 ns and pointer-chased ones 5.74 ns. A single-context harness
would have reported the cheap number for both, with a good fit and a small
error bar.

**Report uncertainty from the fit's own residuals.** Each slope carries a
standard error. That is the number the report gates on, not a separately
measured timer noise floor, because the dominant error is not timer jitter: it
is per-kernel systematic bias -- code alignment, which slots share a cache
line, what the indirect-branch predictor has learned. That bias is fixed for a
given kernel, so repeating a measurement does not reduce it. Two things do:
measuring several *placements* of the same target count (identical work,
different layout), and widening the target-count range, which is why the body
is 512 slots. An opcode whose slope does not clear about 2 sigma is reported as
costing what the baseline costs, and prices there. In an interpreter where
dispatch dominates, that is the correct answer for most of RV32IM, and a
schedule that invents distinctions below that line is writing noise into a
consensus constant.

**Interleave the measurements.** All points in a sweep are run round-robin and
reduced by median, so slow drift on a shared or throttled machine spreads
across every point instead of aliasing into the slope.

## Comparing runs

The output is meant to be compared -- across candidate validator machines, and
across versions of the interpreter as it changes -- so every run records what
it measured and on what.

Each JSON carries a `provenance` block: host, CPU model, kernel, compiler,
whether the build was optimized, the source revision (the script stamps it from
git, `-dirty` included), and an **engine fingerprint**.

The fingerprint is a hash of what the lowering *does*: the gas schedule, the
engine's structural limits, and which ordered instruction pairs it fuses,
probed by lowering every pair from a fixed corpus and recording whether the head
slot came back rewritten. It is derived from behaviour rather than from a source
digest on purpose. A source hash changes when a comment does, and misses a
change that reaches the engine through a header it did not cover. This changes
exactly when the measured engine changes in a way that can move the numbers --
a fusion rule added or removed, a gas cost edited, a run limit moved -- and not
otherwise. It is also independent of build layout, since it records whether a
pair fused and never a handler address: the same engine fingerprints identically
under clang and gcc, at -O0 and -O2. That is what lets a comparison distinguish
"different machine" from "different engine".

`rv32_gas_report.py` checks it. Comparing or merging runs whose fingerprints
differ prints a warning that the numbers describe different machines-under-test;
so does a run that predates fingerprinting, or one built without optimization.
Different *hosts* are noted rather than warned about, since mixing hosts
deliberately is the intended workflow for setting a schedule.

The practical loop when you change `threaded_machine.h`:

```sh
scripts/rv32_gas_calibrate.sh build-release-ccache-tmp --out /tmp/gas-after.json
scripts/rv32_gas_report.py /tmp/gas-before.json /tmp/gas-after.json
```

If the fingerprints differ, the interpreter changed and the old numbers are
stale -- which is the answer you wanted. If they match, the diff is measuring
your machine's mood, not your patch.

## The periodic check

A calibration does not fail loudly when it goes stale. The interpreter moves
underneath it and the numbers keep looking reasonable, which is why staleness
has to be checked mechanically rather than remembered.

`scripts/rv32_gas_check.sh` is that check, meant for a periodic job on a
reasonably quiet machine. It runs a calibration and compares it against
`reference.txt`, committed next to this file:

```sh
scripts/rv32_gas_check.sh build-release-ccache-tmp                     # exits non-zero if broken
scripts/rv32_gas_check.sh build-release-ccache-tmp --fingerprint-only  # "did the engine change?", instant
```

It checks two things.

**The engine fingerprint.** Pure equality, no timing, so it cannot flake. This
catches the likeliest failure: a fusion rule or a gas cost is edited and the
calibration behind the current schedule silently keeps describing the old
interpreter. A mismatch fails the check -- the lowering's decisions moved, so
the old numbers are definitely about a different engine.

**The engine source digest.** The fingerprint records what the lowering
*decides*; it says nothing about how it does it. Rewrite a handler body, change
the dispatch path, reorder a slot, and the fusion matrix, gas table and limits
are all identical while the timings move -- a false negative in the direction
that matters, since the stale calibration keeps looking current. So CMake also
hashes the interpreter's sources into the binary. A mismatch *warns* rather than
fails: unlike a fingerprint change it does not prove the numbers moved, and
failing on every edit would leave a periodic job red through normal development
until somebody regenerated the reference, which is how a job stops being read.

**The guarded costs.** How expensive each case is relative to the **median
saturated cell** *measured in the same run*. Every usable cell is guarded: with
fusion off the whole ISA reproduces to 0.12% median and 2.90% worst across two
back-to-back bench runs, so there is no noisy population left to exclude. The
tolerance is 100% -- a guarded cost has to double before it fires. This is an
alarm for things being wildly out of whack, not a performance budget.

The median rather than a named opcode, because one cell carrying every ratio is
one cell whose misbehaviour is indistinguishable from the whole ISA moving. On
the bench host that happened: every ratio scaled by 2.015x between two runs
while the opcodes held to 0.1%, because the divisor -- `ADD` -- was both
silently fusing and, on that microarchitecture, genuinely about twice as fast
as `AND`. A median over ~100 cells cannot be moved by one of them.

Ratios rather than nanoseconds make the reference portable across **identical**
hardware, which is what the bench pool is. They do **not** make it portable
across microarchitectures, and it is worth being exact about that because the
obvious reading is too optimistic: a reference taken on one cloud host failed 3
of 18 guards on another, with the same engine fingerprint. The baseline is
dispatch-bound and scales with core width and clock, while a chained multiply or
a pointer-chased load is latency-bound and does not, so the two do not move
together and their ratio is not invariant. Regenerate the reference on the
machine class the check will run on; `mode=reference` in the workflow exists for
exactly that.

**The interaction bound.** A program must cost no more than the sum of its
opcodes' individual worst cases, or per-opcode worst-case pricing does not bound
it. That is a property of the method, not a drifted number, so it fails the
check on its own terms.

It also warns, without failing, when a cell has become expensive that is not in
the reference at all. A newly expensive instruction is exactly what a gas
schedule needs to hear about, but on a new host it may just be the first look.

In CI it runs as the **RV32 Gas Calibration** workflow (nightly, plus manual
dispatch; `scripts/schedules/rv32_gas_calibration.toml`), pinned to one bench
host so successive runs stay comparable. Its `mode=reference` option measures
without gating and prints a reference file for that host -- which is how a new
bench host gets bootstrapped, since the committed reference's ratios were
measured somewhere else.

Every run files its calibration JSON to `metrics_history` under
`metrics/rv32-gas-calibration/`, passing or failing, so the costs accumulate as
a time series instead of a nightly pass/fail. Each sample is named for the time
it was measured rather than the commit's, since the nightly re-measures whatever
main last landed and every run is a separate sample of the machine. That is also where the input to a
gas-schedule proposal comes from: `--schedule` takes several of those files at
once and merges them, so proposing a schedule reads history rather than needing
shell access to the bench host.

Regenerate the reference only once you understand what moved and have decided
the schedule does or does not need to follow:

```sh
scripts/rv32_gas_calibrate.sh build-release-ccache-tmp --out /tmp/gas.json
scripts/rv32_gas_report.py /tmp/gas.json --emit-reference > \
  hogd/hog_exec/runtime/rv32/gas_calibration/reference.txt
```

Regenerating to make a failure go away hides the one thing the check exists to
surface.

## Reading the numbers

`ns_per_instruction` is what one more instance of that opcode costs the engine,
in that context, on that host. `slope_stderr_ns` is its uncertainty. A cost is
worth acting on when the slope clears a couple of standard errors *and* the
same ordering shows up on another host.

`--predict` is the check that the per-opcode decomposition is *true*. Each cost
was measured in isolation by substituting one opcode into an otherwise fixed
body; nothing in that procedure verifies the costs add up over a program that
mixes them. This sums them over each validation workload's opcode histogram and
compares against that workload's measured time.

The honest additivity test is the workload that neither fuses nor chains --
`alu_mix`. On the development host it predicts within **2%**, which is the
evidence that the decomposition means something.

The other rows are expected to miss, and how they miss is the point:

- Fused workloads (`address_math`, `load_use`, `branch_dense`) come in *under*
  the prediction, because they retire the same instructions in fewer dispatches.
- Chained ones (`multiply_mix`, `divide_mix`) come in *over* a prediction built
  from independent-context costs, because chaining is the expensive end of those
  opcodes' range. That is the context axis showing up end-to-end.

`word_copy` is the interesting one: it fuses and still overshoots by ~69%.
Fusion can only remove dispatches, so something the per-opcode model cannot see
is dominating -- here, a store-to-load chain, since each store writes the
address the next load reads. **The harness has no context that measures
store-to-load forwarding.**

That is a gap in the model. Whether it is a gap in the *pricing* is a different
question with a different yardstick, and the two are easy to run together. The
residual is measured against costs in the arrangement the workload uses, so it
says the model does not explain the program. What worst-case pricing actually
promises is the `headroom` column: that a program costs no more than the sum of
its opcodes' *individual worst cases*. On this host the pair measures 3.35 ns
against 5.71 + 1.21 = 6.92 ns for a worst-case `LW` plus a worst-case `SW` --
subadditive, 52% under. So the bound holds, and it holds by the method rather
than by luck.

That axis is now swept rather than inferred. A **pair substitution** puts two
adjacent instructions into the body instead of one, so the sweep's slope is
nanoseconds per pair, and two kinds that differ only in whether the load reads
the address the store just wrote isolate the forwarding cost with instruction
mix and register chain held equal:

| pair | ns | vs a worst-case `SW` + worst-case `LW` |
|---|---:|---:|
| `sw;lw distinct` (control) | 1.42 | 81% under |
| `sw;lw forward` | 1.87 | 76% under |

So store-to-load forwarding costs about **0.45 ns per pair**, roughly 0.7x a
baseline instruction, and the forwarding pair is nowhere near superadditive.
Per-opcode worst-case pricing bounds it with room to spare.

That settles this interaction, not all of them. A pair costing more than its
opcodes' individual worst cases sum to would make the method unsound rather than
imprecise, so `--predict` checks every workload and every pair against that
bound and `--check` fails on a violation. The same machinery takes any new pair
kind, which is how the next suspected interaction should be settled -- by
measuring it, not by arguing it is covered.

`--workloads` is the check that matters most for pricing. It runs realistic loop bodies,
including deliberately fusible ones, and reports nanoseconds of real work per
unit of gas charged. A gas limit converts to a wall-clock bound at the *worst*
ns/gas in that set, so the max/min spread is the capacity a schedule gives
away. Watch that number across schedule changes: it is the one that says
whether a change to the table made pricing better, regardless of what any
individual opcode's cost did.

Gas is charged per decoded instruction before fusion, so fusion only ever makes
real cost lower than charged cost. That is the safe direction, and the workload
rows quantify how much headroom it buys.

One asymmetry is worth knowing about. The estimator attributes the whole
difference between target and filler to the target, which is sound when the two
are similar and strains when they are not: `LUI` writes an immediate and reads
no registers, while the filler reads two and writes one, so substituting it
removes more work than `LUI` itself costs and the derived per-instruction figure
can land below one dispatch. Nothing costs less than one dispatch, so
`rv32_gas_report.py` floors every opcode at the baseline. The strain is at the
cheap end, where the answer barely matters -- an opcode a shade cheaper than
ADD is not a pricing risk -- and the expensive end, which is what gas has to
get right, is where the estimator is at its strongest.

## Limits worth knowing

- Costs are per-host. The schedule is a protocol constant, so it has to be set
  from the slowest supported validator class, or from the worst across a set of
  hosts. This harness measures one machine; comparing runs is what
  `rv32_gas_report.py before.json after.json` is for.
- The contexts are the ones we thought of. They are a lower bound on the worst
  case, not a proof of it. A new fusion rule, or a new engine, can create a
  context that is not measured here -- which is the argument for re-running this
  after interpreter changes, not only before schedule changes. `--predict` is
  the tripwire for the class of gap that shows up as a workload the per-opcode
  costs cannot explain; store-to-load forwarding is the one it has already
  found.
- Independent divides sit under the engine's own dispatch cost, so their
  marginal cost is small. That is a real property of this interpreter, not a
  measurement artifact, and it is why the dependent form is measured separately.
- `JAL`, `JALR`, `FENCE`, `ECALL`, `EBREAK`, and `UNIMP` are not measured. The
  first two need a per-slot rewritten target and the rest end the run, so
  neither fits the substitution design. They need their own kernel shapes.
- Syscalls are not measured yet. The sweep machinery generalizes to them
  directly -- the independent variable becomes the number of calls at a fixed
  argument size (giving a base cost) and then the argument size at a fixed call
  count (giving a per-byte cost), which is the `base + per-byte` shape
  `gas_cost_table.h` already uses for SHA256.

## Changing the schedule

`--schedule` prints a proposal. It is not a patch. Every cost in
`kInstructionGasSchedule` is folded into `rv32::kGasScheduleDigest`, so changing
one is a coordinated gas-schedule update: move the pinned digest in
`artifact.h` and regenerate the committed RV32 artifacts in the same change, or
every deployed artifact stops validating.

## Files

| File | Role |
|---|---|
| `kernel.h/.cc` | Kernel synthesis: register plan, filler, target placement, contexts |
| `lowering_audit.h/.cc` | Checks the production lowering matches the kernel's source |
| `measure.h/.cc` | Interleaved timing, median/MAD, OLS + Theil-Sen fit with standard error |
| `provenance.h/.cc` | Host/build metadata and the behavioural engine fingerprint |
| `workloads.h/.cc` | Realistic loop bodies for the end-to-end ns/gas check |
| `rv32_gas_calibrate.cc` | CLI, sweep plan, JSON output |
| `rv32_gas_calibration_test.cc` | Correctness net for all of the above |
| `reference.txt` | Committed expectations for `scripts/rv32_gas_check.sh` |
