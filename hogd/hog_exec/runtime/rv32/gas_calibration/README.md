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

**Synthesize the design; do not fit one.** For each opcode, build a family of
kernels that are byte-for-byte identical except for how many body slots hold
that opcode instead of a filler instruction. Same body slot count, same code
size, same trip count, same register plan, same boundaries. Fit time against
the target count and the slope is the cost difference between the target and
the filler, in nanoseconds, directly. There is no design matrix to condition
and nothing to be collinear with, because only one thing varies. All the fixed
overhead lands in the intercept and cancels.

**Anchor the slopes.** A separate sweep varies the body slot count with an
all-filler body, giving the filler's own absolute cost. Adding it back turns
each relative slope into absolute nanoseconds per instruction.

**Suppress fusion, and check that you did.** Targets are placed non-adjacently
and separated by an R-type `add` filler on private registers, which no fusion
rule matches in either position. That is a claim about the lowering, so
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
interpreter.

**The guarded costs.** How expensive the worst cases are relative to the
baseline instruction *measured in the same run*. Ratios, not nanoseconds, so
the reference survives moving to different hardware. Only cells at or above
1.5x baseline are guarded, because those are the ones that reproduce tightly;
guarding the near-baseline population would fail most nights on drift far too
small to matter, and a job that cries wolf gets muted and then catches nothing.
The tolerance on top of that is 100% -- a guarded cost has to double before it
fires. This is an alarm for things being wildly out of whack, not a performance
budget.

It also warns, without failing, when a cell has become expensive that is not in
the reference at all. A newly expensive instruction is exactly what a gas
schedule needs to hear about, but on a new host it may just be the first look.

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

`--workloads` is the check that matters most. It runs realistic loop bodies,
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
  after interpreter changes, not only before schedule changes.
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
