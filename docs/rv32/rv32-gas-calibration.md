# RV32 Gas Calibration

How the RV32 instruction gas schedule gets set from measurement instead of
guesswork, and what the first run found.

The harness, its method, and its limits are documented next to the code in
`hogd/hog_exec/runtime/rv32/gas_calibration/README.md`. This page is the
operational record: how to run it, what it said, and what follows from that.

## Status

The measurement exists; the schedule has not changed. `kInstructionGasSchedule`
in `program.h` still carries its placeholder costs (1 for most opcodes, 3 for
multiply, 8 for divide). Changing any of them is a coordinated gas-schedule
update -- the costs are folded into `rv32::kGasScheduleDigest`, so the pinned
digest in `artifact.h` and every committed RV32 artifact move in the same
change -- so the numbers below are input to that decision, not a patch waiting
to land.

## Run it

```sh
B=build-release-ccache-tmp   # your Release build tree
scripts/rv32_gas_calibrate.sh $B --out /tmp/gas-1.json
scripts/rv32_gas_calibrate.sh $B --out /tmp/gas-2.json
scripts/rv32_gas_report.py /tmp/gas-1.json /tmp/gas-2.json            # what moved
scripts/rv32_gas_report.py /tmp/gas-1.json /tmp/gas-2.json --schedule # merged
scripts/rv32_gas_report.py /tmp/gas-1.json --workloads
scripts/rv32_gas_report.py /tmp/gas-1.json --syscalls                 # the ecall seam
```

The build directory is a required argument -- it is not searched for and there
is no default, because which tree the binary comes from decides what is
measured. The script builds only the `rv32_gas_calibrate` target into it, and
only the RV32 ISA core is linked, so the build is small. A full run takes a few
minutes.

The build configuration is part of the measurement: Release is `-O3`, `-march`,
libc++ under clang, and LTO across the interpreter's translation units, and an
ad-hoc `-O2` build measures a different interpreter while looking perfectly
reasonable. CMake stamps the build type and flags into the binary, every run
records them, and the harness refuses to measure a non-Release build.

Run it more than once. `--schedule` accepts several runs and merges them by
taking the highest cost each context reached while marking any opcode that not
every run could tell apart from the baseline. That is not belt-and-braces: see
the stability section below for why one run is enough to find the large effects
and not enough to price the small ones.

## What is measured

`ThreadedMachine`, tracing off, built optimized with LTO: the engine and the
codegen the node runs. Per opcode and operand context, the harness times a loop
body made **entirely of that opcode** and divides by the instruction count. The
result is what a program consisting of that instruction costs per instruction.
It reports the worst context.

That is a level, not a difference, and the distinction is the whole method. The
harness used to measure the *marginal* cost of one more instance of an opcode
against a body of filler -- a contrast. A contrast between two large timings
goes negative as soon as layout noise rivals the effect, and it did: about a
third of sweeps priced below zero, which is not a cost a gas schedule can use.
Dividing one absolute timing by one instruction count cannot produce a negative,
does not depend on what the filler costs, and asks the question pricing actually
cares about -- if a block is filled with this instruction, what does each one
cost?

Saturating a body means adjacent copies of the opcode, which is exactly the
shape a fusion rule matches. The harness lowers those bodies with fusion turned
off (`PreInstantiation::create`'s `fuse` flag, defaulted true and untouched by
production) and measures the unfused cost directly. That is also the cost the
schedule charges: fusion is a saving the engine may or may not get, and
worst-case pricing cannot assume it.

Detecting fusion after the fact was the earlier approach and it has a blind
spot the flag removes. The audit compares each body slot against the same word
lowered *in isolation* -- but the isolated probe is `filler, target, filler`,
and the filler is an `ADD`, so for target `ADD` the probe fuses exactly like
the body does, both sides match, and the audit reports clean. The one cell the
whole reference used to divide by was the one cell the audit structurally could
not check. With the flag, all 100 single-target cells saturate rather than 98:
`ADDI` is measurable for the first time, having declined only because
`addi;addi` is a rule.

A pair row is the opposite -- `auipc;jalr` exists to measure the fused form --
so those keep fusion on, and their audit requires the rule to fire.

Ratios are taken against the **median** saturated cell, not against `ADD`. One
cell carrying every ratio is one cell whose misbehaviour is indistinguishable
from the whole ISA moving, and on the bench host that is what happened: between
two runs every guarded ratio scaled by 2.015 while the opcodes themselves held
to 0.1%. `ADD` was both the divisor and, on that microarchitecture, genuinely
about twice as fast as `AND` -- a property of the generated code there, not
present on a Xeon. A median over ~100 cells cannot be moved by one of them, and
still moves when most of the ISA does, which is what the gate is for.

The contexts are the load-bearing part. An opcode does not have one cost, and
which end of its range applies is the guest author's choice, so gas has to
price the end an adversary can force.

## How reproducible is it

Two back-to-back runs on the bench host (`hog10`, AMD EPYC 9554, Release built
with gcc), as a relative difference between the two runs across the 95 cells
both measured:

| | median | p90 | max |
|---|---:|---:|---:|
| saturated, fusion off, median-normalized | **0.12%** | **1.22%** | **2.90%** |

No cell moves more than 3%, against a gate tolerance of 100%. For contrast, the
same two-run comparison before fusion was disabled and while ratios were taken
against `ADD` had *every* cell scaling by 2.015x -- the gate would have failed
wholesale on a machine that had not changed.

The expensive cells agree between runs to within a few tenths of a percent:

```
                    run 1 / run 2   (x the median cell)
LH   chase           2.648 / 2.651
JALR chain           2.607 / 2.610
LW   chase           2.491 / 2.496
DIV  chain           1.979 / 1.980
REM  chain           1.648 / 1.651
BNE  taken           1.416 / 1.416
```

That host is the one that made the contrast unusable -- back-to-back runs there
moved `SUB` by 6.3 ns and priced 28% of the ISA below zero. The same machine,
the same binary and the same sweeps now agree to a tenth of a percent.

## First results

Host: 4-core Intel Xeon @ 2.80GHz, 32 KiB L1d and 1 MiB L2 per core, Linux,
Release build (`-O3 -march=native`, LTO). Development container, not a
validator; treat the ordering as the finding and the absolute nanoseconds as
host-specific. These numbers predate the saturated estimator, so the absolute
values are the contrast's; the ordering is unchanged.

Baseline: one dispatched ALU instruction costs about **1.0 ns**, roughly 2.8
cycles. That is the number every other cost is relative to.

The interpreter's dispatch dominates almost everything the guest asks it to do.
Most of RV32IM -- every I-type and R-type ALU op, every load and store with
independent addresses, not-taken branches -- measures within about 30% of that
baseline, and much of it is not distinguishable from the baseline at all. So
the flat cost of 1 is close to right for the bulk of the ISA.

The exceptions are what matter, and they run in both directions:

| Case | Measured | Priced | |
|---|---:|---:|---|
| Pointer-chased `LW` (`lw rd, 0(rd)`, shuffled 64 KiB cycle) | ~5.7 ns | 1 | **~6x under-priced** |
| Dependent `DIV` / `REM` (divide by one, chained) | 5.2 - 6.4 ns | 8 | about right |
| Taken conditional branch | ~2.5 ns | 1 | **~2.5x under-priced** |
| Independent `DIV` / `DIVU` | ~1.3 ns | 8 | ~6x over-priced |
| `MUL` chained / `MULH*` chained | 1.6 - 1.9 ns | 3 | about right |
| Independent loads and stores, any working set | ~1.0 - 1.1 ns | 1 | about right |
| `LUI` | ~0.7 ns | 1 | slightly over-priced |

Three things follow.

**Dependence is the dominant cost axis, not the opcode.** The two worst cases
found are both ordinary instructions arranged so the engine cannot overlap
them: a load whose address is the previous load's result, and a divide whose
dividend is the previous divide's result. Measured independently, the same two
instructions cost 0.98 ns and 1.26 ns; measured dependent, 5.7 ns and 5.2 ns.
A harness that measured each opcode once, in one arrangement, would have
reported the cheap number with a small error bar and a good fit -- and priced
the ISA's two most attackable instructions at a sixth of their worst cost.

**Divide is priced about right, but not for the reason it was priced.** The
placeholder 8 looks like a large overcharge against independent divides at 1.3
ns. Against the dependent form it is close to correct, and for `REM` slightly
low. The current number is right by luck; the measurement says which case it
should be justified by.

**Guest memory being capped at 64 KiB removes a whole class of risk.** The
entire guest address space fits in this machine's L2, so the spread between an
L1-resident working set and one spread over all of guest memory is about 5% for
independent accesses. There is no cold-DRAM case to price. The pointer chase is
expensive because of dependence, not capacity.

## The periodic check

`scripts/rv32_gas_check.sh build-release-ccache-tmp` runs a calibration and compares it against
`hogd/hog_exec/runtime/rv32/gas_calibration/reference.txt`, exiting non-zero if
the calibration no longer describes the engine. It is meant for a periodic job
-- nightly, weekly, whatever cadence suits -- on a reasonably quiet machine, and
it is not built to run on every change.

It checks three things: the engine fingerprint (deterministic, cannot flake), a
set of guarded costs held as ratios to the median cell measured in the same run,
and the interaction bound -- that no program or pair costs more than the sum of
its opcodes' individual worst cases. The cost tolerance is loose by design:
every usable cell is guarded, and a guarded cost has to double before it fires. The bound is not loose, because exceeding it is a failure of
the pricing method rather than a drifted measurement.

The ratios are portable across **identical** hardware, which is what the bench
pool is, and not across microarchitectures: a reference taken on one cloud host
failed 3 of 18 guards on another with the same engine fingerprint. The baseline
is dispatch-bound and scales with core width and clock; a chained multiply or a
pointer-chased load is latency-bound and does not. So bootstrap the reference on
the pool the check runs on -- `mode=reference` exists for that -- and do not
expect one taken elsewhere to hold.

`--fingerprint-only` answers "did the engine change?" instantly, without
measuring, for use by hand after touching the interpreter.

In CI this is the **RV32 Gas Calibration** workflow
(`.github/workflows/rv32_gas_calibration.yml`, driving
`scripts/schedules/rv32_gas_calibration.toml` through `hog_sched`). Manual
dispatch for now; it runs on the bench pool and builds Release with a pinned
compiler, because both the host and the generated code are part of what is
being measured -- the fingerprint is deliberately compiler-independent, so a
compiler change would move the numbers silently.

`mode=reference` measures without gating and prints, in order: the per-opcode
costs, the validation workloads, the additivity check, the **proposed gas
schedule for that host**, and a reference block for it. Use it to bootstrap a
new bench host, whose ratios the committed reference does not describe, and to
get schedule numbers off the machine class that runs the chain rather than off
a development container. `check`, the default, gates.

## Re-running after an interpreter change

Every calibration JSON records two identifiers for the engine, because one is
not enough.

The **engine fingerprint** is a hash of the gas schedule, the structural limits,
and which instruction pairs the lowering fuses, probed from the engine itself
rather than hashed from source text. It is stable across compilers,
optimization levels, and hosts, and moves when the lowering's behaviour moves --
disabling a single fusion rule changes it. A mismatch fails the check.

It has one blind spot, and it is a real one: it records what the lowering
*decides*, not how it does it. A rewritten handler body or a changed dispatch
path moves timings while leaving the fusion matrix, gas table and limits
identical, so the fingerprint sits still and a stale calibration keeps looking
current. The **engine source digest** covers that -- CMake hashes the
interpreter's sources into the binary. Verified: bumping one hot-path constant
in `threaded_machine.h` leaves the fingerprint at `71e9993b` and moves the
digest. A mismatch warns rather than fails, since unlike a fingerprint change it
does not prove the numbers moved, and a check that goes red on every interpreter
edit stops being read.

That makes the check after a `threaded_machine.h` change mechanical:

```sh
scripts/rv32_gas_calibrate.sh build-release-ccache-tmp --out /tmp/gas-after.json
scripts/rv32_gas_report.py /tmp/gas-before.json /tmp/gas-after.json
```

A changed fingerprint means the old numbers are stale and the report says so.
An unchanged fingerprint means any difference is the host, not the patch.

The same check guards the schedule: an old JSON silently outliving the engine
it measured is the failure mode a gas calibration most needs to avoid, because
nothing about the stale numbers looks wrong.

## How reproducible is this

Two full runs, same host, same binary, back to back, compared with
`rv32_gas_report.py before.json after.json`:

- Taken branches moved by 0.6-2.2%. `DIV`/`REM` chained moved by 1.4-7%. These
  are the resolved, expensive cases.
- `AUIPC` moved by 48%. Independent `DIV` moved by 36-41%. Three of the six
  not-taken branches moved by 50-60%.

Both populations are behaving correctly. The unstable cells are the ones whose
cost sits within a few tenths of a nanosecond of the baseline, so an absolute
drift far smaller than the stable cells' effect size is an enormous relative
change. The stable cells are the ones with something real to measure.

The practical rule: a single run is enough to find the mispricings worth acting
on, and is not enough to justify a distinction between two opcodes that both
land near the baseline. Merging runs enforces that -- an opcode only keeps a
cost above the baseline if every run agreed it belongs there.

This host is a shared 4-core development container, which is close to the worst
case for stability. A quiet, frequency-pinned validator-class machine
(`scripts/bench_mode.sh`) should do better, and that is where a run that
actually sets the schedule belongs.

## Pricing quality

Per-opcode costs are inputs. The output that matters is how far apart real
code's nanoseconds-per-gas sit, because a gas limit converts to a wall-clock
bound at the worst of them -- the rest of the range is capacity the schedule
gives away.

`--workloads` measures that over a set of realistic loop bodies, including ones
the lowering fuses. Under the current flat schedule the spread is about **5.4x**
on this host: `divide_mix` charges 0.28 ns of real work per gas while
`word_copy` charges 1.50. `--schedule` prints a proposal and re-prices the same
workloads under it: the merged two-run proposal takes the spread from 5.4x to
about **2.9x**.

Not all of what remains is error. A workload whose loads are independent still
pays the pointer-chase rate, because the schedule is charged at decode time and
cannot know which arrangement it is looking at. Pricing the worst case
necessarily overcharges the best case; the spread that is left after a
calibration is the part of that gap the schedule cannot close, and closing more
of it would take a different mechanism than a per-opcode table.

The fusion rows are worth reading separately. Gas is charged per decoded
instruction, before fusion, so a fused pair is charged for two instructions and
dispatched once. That is the safe direction -- fusion can only make real cost
lower than charged cost -- and the workload rows show it is worth roughly 20-30%
on fusible code.

## The charging model stays a flat per-instruction table

Everything below is measurement. None of it changes the shape of what the
engine charges, and it is not meant to.

`kInstructionGasSchedule` is one cost per opcode, charged per decoded
instruction. That is the model, and the calibration exists to put better
numbers in it -- not to replace it with something that prices pairs, contexts,
or fusion at charge time. Context-sensitive or pair-sensitive gas would make a
program's cost depend on which instructions sit next to each other, which is a
substantially harder accounting model to implement, to audit, and to keep
deterministic across engine versions. It is not on the table.

Two consequences follow, and they are why a flat table is defensible here:

- **Fusion is never priced, and never needs to be.** Gas is charged per decoded
  instruction, before fusion, so a fused pair is charged for two and dispatched
  once. Fusion can only make real cost *lower* than charged cost, so ignoring
  it is the safe direction. It costs capacity -- roughly 20-30% on fusible code
  -- and buys a model that does not have to know the fusion rules at all.
- **The pair sweeps are a check on the flat model, not an extension of it.**
  Charging each opcode its individual worst case bounds a program only if no
  combination is *superadditive* -- costing more than its parts. That is the one
  way a flat table could be unsafe, so it is measured directly rather than
  assumed. Everything measured so far is strongly subadditive (56-82% headroom),
  which is the evidence that the flat table holds.

If a pair ever did exceed its parts, the remedy stays inside the flat table:
the participating opcodes have a worse context than the sweep found, so raise
their cost, or widen the whole schedule by a margin. Adding an interaction term
would be a change to the accounting model and should be argued for on its own,
not adopted as a bug fix.

## Do the costs add up?

Every cost is measured by substituting one opcode into an otherwise fixed body,
which says nothing on its own about whether the costs compose. `--predict` sums
them over each validation workload's opcode histogram and compares against that
workload's measured time.

`alu_mix` -- the workload that neither fuses nor chains -- predicts within
**2%**. That is the evidence the decomposition means something. The rest miss in
the directions the model says they should: fused workloads undershoot, and
`multiply_mix` / `divide_mix` overshoot a prediction built from
independent-context costs because each result feeds the next instruction.

One row does not fit either story, and it is the useful one. `word_copy` fuses
and still comes in ~69% over. Fusion can only remove dispatches, so a cost
outside the per-opcode model is dominating: each store writes the address the
next load reads, so the pair serializes on store-to-load forwarding. **No
context in the harness measures that.**

Whether that is also a *pricing* problem is a separate question, and the two
numbers in the table answer different things. The residual is against costs in
the arrangement the workload uses -- a model-fit diagnostic. What worst-case
pricing promises is the `headroom` column: that a program costs no more than the
sum of its opcodes' individual worst cases. Here the pair measures 3.35 ns
against 6.92 ns for a worst-case `LW` plus a worst-case `SW`, so it is
subadditive by 52% and the bound holds by the method rather than by coincidence.

The sharper question -- whether any interaction can be **superadditive**, costing
more than its opcodes' individual worst cases sum to -- is now measured rather
than argued. A *pair substitution* places two adjacent instructions per
substitution instead of one, so a sweep's slope is nanoseconds per pair, and two
kinds differing only in whether the load reads the address the store just wrote
isolate the effect:

| pair | ns/pair | vs worst-case `SW` + worst-case `LW` (7.66 ns) |
|---|---:|---:|
| `sw;lw distinct` (control) | 1.42 | 81% under |
| `sw;lw forward` | 1.87 | 76% under |

Store-to-load forwarding costs about **0.45 ns per pair** -- real, measurable,
and nowhere near superadditive. Worst-case per-opcode pricing bounds it.

`--predict` now checks every workload and every pair against that bound and
`--check` fails on a violation, because a superadditive interaction would make
the method unsound rather than merely imprecise. It also lists the programs with
the most headroom, which is the opposite failure: `load_use` is billed 8.3x what
it costs, `branch_dense` 4.1x. Safe, but that is capacity the schedule gives
away, and it is where a cheaper context would pay for itself.

## Before changing the schedule

- Take the numbers from the bench pool, not from wherever you happen to be
  working. The calibration workflow's `mode=reference` prints the proposal
  under `===== proposed gas schedule from this host =====`, measured on the
  machine class that runs the chain. A schedule read off a development
  container is measuring the wrong computer.
- Re-run on the slowest supported validator class, not a development host. The
  schedule is a protocol constant and has to hold on the slowest machine that
  must keep up.
- Re-run on more than one microarchitecture. The finding to trust is a cost
  ordering that survives a change of host; an absolute nanosecond figure is not
  portable.
- Decide the resolution deliberately. Most of the ISA sits within a factor of
  two of the baseline, so a unit of one baseline instruction rounds nearly
  everything to the same value. `--resolution` splits the baseline into several
  gas units, which keeps rounding loss bounded, at the cost of scaling every
  number in the table -- including block gas limits and the syscall costs in
  `gas_cost_table.h`, which have to scale with it.
- Price `JAL`, `FENCE`, and the trapping opcodes. The substitution design
  cannot hold them constant, so they are unmeasured and would keep placeholder
  costs. (`JALR` was in this list until a fall-through kernel shape made it
  measurable.)
- Extend the pair sweep to other suspected interactions. The mechanism is in
  place and store-to-load forwarding is measured; the same shape would let the
  harness *measure* fusion's benefit directly instead of inferring it from a
  slot count. That is a better number for the report, not a new term in the
  schedule -- see the note on the charging model below.
- Regenerate `reference.txt` on the bench pool. The committed one was taken on a
  development container and will not hold there.
- Re-run after interpreter changes, not only before schedule changes. A new
  fusion rule or a dispatch change moves these numbers, and a schedule
  calibrated against the old engine silently loses its margin. The engine
  fingerprint is there to make that detectable rather than remembered.

## Syscalls

Still unpriced: `gas_cost_table.h` prices one syscall family (SHA256) and
returns zero direct host gas for the rest. What is now measured is the half of
that cost every syscall shares -- the **ecall seam**, the crossing itself --
which the harness reports under `--syscalls` and the calibration workflow prints
on the bench host.

### What was measured, and what was not

In: the ecall's suspend, the unwind to the driver, the virtual dispatch through
`Host::ecall`, the gas debit and register commit on the way back, and the fresh
`enter_run` the resumed guest re-enters through.

Out: the production syscall path. `syscall_entry`'s ABI parse, the guest-pointer
bounds checks, the coroutine frame every call allocates, and
`SystemCallHandler`. That path needs the protocol XDR, the DB proxy and the
snapshot cache; the calibration harness links only `rv32_isa_core`, deliberately,
so a calibration can run on a candidate validator in seconds without configuring
the node build. Keeping that property is worth more than folding the two halves
into one binary, so **read these as a floor under a syscall's fixed cost, not as
a syscall's cost.**

### First results

Host: the same 4-core Xeon development container as the opcode numbers above,
pinned with `taskset` and `setarch -R`. Treat the ratios as the finding and the
nanoseconds as host-specific.

| shape | ns/call | x the baseline instruction | charged | models |
|---|---:|---:|---:|---|
| `seam` | 17.6 | 11.2 | 1 gas | the crossing alone |
| `selector_reload` | 17.1 | 10.8 | 2 gas | the bridge ABI's minimum |
| `out_pointer_8` | 18.1 | 11.5 | 2 gas | `GetBlockHeight` |
| `out_pointer_32` | 19.9 | 12.6 | 2 gas | `GetCurrentAddress`, `GetMsgSender` |
| `two_out_pointers` | 17.9 | 11.4 | 2 gas | `GetCalldataLen` |

Three things follow, and the first is the one that matters.

**The crossing costs about eleven instructions and is charged one.** Before any
handler runs, before a byte is hashed or a storage key is read, leaving the
guest and coming back costs what eleven dispatched ALU instructions cost. The
schedule charges the `ecall` one gas, `kNoSyscallCost` is 10, and most syscalls
declare no direct host gas at all. Whatever the eventual per-syscall numbers
say, this constant is under all of them.

It is not the run barrier. A taken conditional branch also ends a straight-line
run and re-enters through `enter_run`, and costs **2.78 ns** on this host. The
seam costs 17.6. So roughly 15 ns of it is the crossing proper -- suspend,
unwind, virtual call, apply, re-enter -- and not the run split that any branch
also pays.

**Output size is nearly free.** Going from no host-side write to a
bounds-checked 32-byte write moves the cost by about 17%; splitting eight bytes
across two pointers instead of one is inside the noise. Whatever term a
stateless read syscall's price needs, "bytes returned" is not it -- which is
worth knowing before anyone writes a per-byte cost for `GetCurrentAddress`.

The same goes at the other end: `seam` and `selector_reload` differ by one
`addi`, and they land within the run-to-run band of each other -- in this run
`seam` is nominally the *dearer* of the two. Do not read an ordering into that.
Read it as the crossing swamping a whole ALU instruction, which is the same
finding from the cheap side.

**The cost is genuinely constant per call.** The sweeps fit a line in call count
at R² 0.991-0.997, and the saturated level agrees with the fit's slope to within
a few percent. A crossing costs what a crossing costs, however many of them a
loop makes.

### How reproducible is it

Two full runs back to back on the container, per shape: **0.87%, 0.87%, 0.98%,
2.46%, 2.91%**. That is in the same band as the opcode harness's stable cells,
on a machine the opcode doc calls close to the worst case for stability. The
seam is a large effect measured against a small baseline, which is the easy
case.

### Gating

`--emit-reference` now writes `guard_syscall <shape> <ratio>` lines, checked
under the same tolerance as the opcode guards and with the same rule that an
unmeasured guard is a failure rather than a pass. The committed `reference.txt`
has none yet -- it was taken on the bench host before this existed, and a
reference with no seam guards checks no seam guards rather than failing. They
appear the next time the bench pool regenerates it, which is the same
`mode=reference` run that would be needed to make the ratios portable anyway.

### What comes next

The remaining half is a second, runtime-linked harness driving the real
`syscall_entry`. It cannot live in `gas_calibration` without giving up the
core-only property, so it belongs in a sibling target that links
`rv32_syscall_bridge` and the runtime; the provenance, JSON and report machinery
is in the `rv32_gas_calibration` library rather than the executable, so it is
shareable across the two.

Then the numbers compose, and the composition is the check worth running:
measured end-to-end cost of a real `GetBlockHeight` against (seam) + (handler).
Agreement within a few percent means the decomposition is real and
`base + per-byte` is a defensible shape for the table. Disagreement is the ABI
parse and the coroutine frame, and says how large a third term has to be. That
is the same discipline `--predict` already applies to the opcode costs, and it
is what makes a syscall price arguable rather than merely measured.

Two smaller follow-ons:

- **`SHA256`'s per-byte cost belongs in Google Benchmark, not here.** It is
  native C++ with no interpreter dispatch in it and no arrangement a guest can
  force, so the whole apparatus this harness spends on holding a guest
  arrangement constant buys nothing. `hogd/crypto` already has a benchmark
  target; a size range with `SetBytesProcessed` is a few lines, and
  `scripts/compare_benchmark_json.py` already diffs the output.
- **The engine fingerprint should cover the syscall gas schedule.** It currently
  hashes the instruction schedule, the structural limits and the fusion matrix.
  `kSyscallGasCostSchedule` is already folded into `kGasScheduleDigest`, so the
  input exists; and `HOGD_RV32_ENGINE_SOURCES` wants a syscall-path equivalent,
  or a rewrite of `syscall_entry_outer.h` moves these numbers with nothing
  saying so.
