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

Measured now, in two harnesses that meet in the middle:

- the **ecall seam** -- the crossing itself, what every syscall pays to leave
  the guest and come back -- in the calibration harness under `--syscalls`;
- the **production syscall path** -- `syscall_entry`, the ABI parse, the
  coroutine frame, the real `SystemCallHandler` -- in
  `rv32_syscall_entry_bench`, a Google Benchmark binary that links the runtime.

The split is not incidental. The calibration harness links only
`rv32_isa_core` so it can run on a candidate validator without configuring the
node build, and `syscall_entry` needs the protocol XDR, the DB proxy and the
snapshot cache. Keeping them apart preserves that property and yields two
independent estimates of the seam, which is how the decomposition below gets
checked rather than assumed.

The pricing has not changed: `gas_cost_table.h` still prices one syscall family
(SHA256) and returns zero direct host gas for the rest. What follows is the
input to changing that.

### The seam

What the calibration harness measures: the ecall's suspend, the unwind to the
driver, the virtual dispatch through `Host::ecall`, the gas debit and register
commit on the way back, and the fresh `enter_run` the resumed guest re-enters
through. Not a syscall -- a floor under one. The syscall itself is two sections
down.

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

### The other half, measured

`rv32_syscall_entry_bench` (Google Benchmark,
`hogd/hog_exec/runtime/rv32/rv32_syscall_entry_bench.cc`) measures what the
seam harness cannot: `syscall_entry` itself, driven exactly as
`rv32_contract.cc` drives it -- live guest memory, the real ABI parse, the
coroutine frame every call allocates, the real `SystemCallHandler`. It is a
separate binary because that path needs the protocol XDR, the DB proxy and the
snapshot cache, and keeping the calibration harness core-only is worth more
than one binary.

Two rows per syscall, so the halves can be checked against the whole rather
than assumed to compose: `entry/` calls `syscall_entry` directly, `vm/` drives
the same syscall end to end through `ThreadedMachine`.

Same container, pinned to one core, five repetitions, medians:

| syscall | `entry` ns | ± | `vm` ns/call | difference | gas charged |
|---|---:|---:|---:|---:|---:|
| `GetBlockHeight` | 90.4 | 0.29 | 106.8 | 16.4 | **0** |
| `GetCurrentAddress` | 92.0 | 0.65 | 106.2 | 14.1 | **0** |
| `GetMsgSender` | 92.1 | 0.29 | 107.0 | 15.0 | **0** |
| `GetSourceTransactionHash` | 91.0 | 0.60 | 106.0 | 15.0 | **0** |
| `GetCalldataLen` | 93.0 | 0.67 | 105.4 | 12.3 | **0** |
| `SHA256` (0 bytes) | 172.9 | 0.69 | 184.4 | 11.5 | 500 |
| `SHA256` (1 KiB) | 914.7 | 4.17 | 920.9 | 6.1 | 102,900 |

**A stateless read costs about 90 ns and is charged nothing.** The five differ
by 3% -- less than the spread between two runs of some opcode cells. Whatever
each handler does is lost in the entry path around it, which is the finding:
there is no per-syscall cost to price here, there is one crossing cost that
every one of them pays. At the baseline instruction measured on this host, 90 ns
is roughly **sixty dispatched instructions**, charged as zero.

**The seam composes.** The `vm` − `entry` difference is 12-16 ns, against the
17-20 ns the seam harness measures directly. The residual is the right sign and
size: the seam harness saturates its body with ecalls so every one is its own
straight-line run, while this guest loop has four instructions between calls to
amortize `enter_run` over. Two harnesses, two methods, one number -- which is
the evidence that the decomposition is real rather than a story about it.

**SHA256 is priced in the wrong shape.** Measured, it is
**173 ns + 0.72 ns/byte**. Charged, it is `SHA256HashCost` = 500 + 100/byte. The
base is about 3x over-priced against instruction gas; the per-byte term is
about **140x** over. Within the one syscall the schedule actually prices, real
nanoseconds per gas run from 0.346 at zero bytes to 0.0073 at 32 KiB -- a **47x
spread**, wider than the 5.4x the whole opcode schedule had before calibration.

That is not a rounding error to tidy up. Two syscalls whose costs differ by 5x
can be billed the same, or the same syscall billed 47x differently depending on
an argument the caller chooses, and a block limit has to be sized for the worst
of it.

### Setting the costs

```sh
B=build-release-ccache-tmp   # your Release build tree

# The opcode calibration first: it supplies the gas unit.
scripts/rv32_gas_calibrate.sh $B --out /tmp/gas.json

# Then the syscalls, on the same host and the same engine.
scripts/rv32_syscall_bench.sh $B --out /tmp/sys.json

scripts/rv32_syscall_report.py /tmp/sys.json                    # what it costs
scripts/rv32_syscall_report.py /tmp/sys.json --gas /tmp/gas.json --schedule
scripts/rv32_syscall_report.py before.json after.json           # what moved

# An independent check on the two primitives that do real work.
scripts/rv32_openssl_speed.sh --out /tmp/openssl.json
scripts/rv32_syscall_report.py /tmp/sys.json --crosscheck /tmp/openssl.json
```

`--schedule` refuses without `--gas`, and refuses a `--gas` file whose engine
fingerprint or host does not match the benchmark's. That is not ceremony. Gas is
one currency: a syscall's price has to be in the same unit as an instruction's,
that unit is the baseline instruction the opcode calibration measured, and
joining two hosts' numbers produces a complete, plausible schedule that is wrong
by whatever the two machines differ by. The check is the only thing standing
between a careful measurement and a confidently mispriced one.

### Checking the measurement against something else

The benchmark times hogd's own code, which means a mistake in the harness
surfaces as a plausible number rather than as a failure. That is not
hypothetical: a `VerifySignature` row shipped verifying a signature over the
wrong message, took ed25519's cheap rejection path, and reported it as the cost
of verifying. Numbers that look reasonable are the failure mode to design
against.

`scripts/rv32_openssl_speed.sh` runs `openssl speed` on the same host with the
same pinning, and `--crosscheck` puts the two side by side:

```
cross-check against openssl speed (OpenSSL 3.6.3 9 Jun 2026)
openssl built for: -march=x86-64 -mtune=generic
  primitive                openssl        hogd    ratio  verdict
  SHA256 ns/byte            0.5450      0.5396    0.99x  ok
  ed25519 verify ns          96104       28635    0.30x  ok
```

That is hog10, run 33469060485.

The two halves are worth very different amounts, and the report says which is
which rather than presenting one number.

**SHA256 is the sharp check.** `hogd/crypto/crypto.h` calls
`SHA256_Init/Update/Final`, so the syscall and `openssl speed sha256` run the
same compression function out of the same libcrypto. The per-byte terms should
agree to a few percent, and they do. A real gap here means the benchmark is
timing something other than hashing.

**ed25519 is a backstop, not a validation,** and the 0.30x above is worth
understanding rather than waving through. Both halves run out of one libcrypto
on one host, yet one matches to 1% and the other is 3.4x apart, because the two
primitives are built differently *inside the same binary*: OpenSSL's SHA256 has
hand-written SHA-NI assembly dispatched at run time, which `-march` cannot
suppress, while its Ed25519 is generic C with no vectorised backend to dispatch
to -- so the distro build's `-march=x86-64 -mtune=generic` binds it, and hogd's
dalek gets `-C target-cpu=native` on a Zen 4 EPYC. The cycle counts confirm it
exactly: 96.1us is ~298k cycles at 3.1GHz, textbook ref10 portable C, and
28.6us is ~89k, textbook dalek AVX2/AVX512. Nothing is wrong with either
number. A factor apart is expected and the tolerance has to allow it. What it still catches is a row that is not doing the
work at all: hoisted out of the timing loop, measuring a no-op, wired to the
wrong syscall. Those land orders of magnitude out. The rejection path that
actually bit us is caught earlier and far more sharply -- verification fails,
the syscall returns an error, and the benchmark skips the row instead of timing
it.

The two files must come from the same machine, and the report refuses the pair
when they do not -- the same rule `--gas` enforces, and for the same reason. A
ratio across two hosts is not a weaker check but a misleading one: both numbers
stay individually plausible and only their ratio is nonsense. This is not
hypothetical. A development container migrated to different silicon
mid-session, gaining SHA-NI, and a fresh openssl run against a benchmark from
the previous host read as a 4x fault in hogd.

A failed cross-check prints `INVESTIGATE` and still exits 0. It is a sanity
check on a measurement, not a gate on one: an OpenSSL upgrade on the bench host
can move these ratios without anything being wrong with hogd, and failing the
job over that would cost the opcode calibration running beside it.

The **RV32 Gas Calibration** workflow runs both binaries on the bench pool and
prints the proposal there, which is where a schedule that becomes a protocol
constant should come from rather than from a development container. Both JSONs
are filed to metrics history, separately, joined at read time by fingerprint --
so a syscall run can be compared against an opcode calibration from a different
night on the same silicon.

### What the proposal says

Measured on the bench pool -- hog10, AMD EPYC 9554, the machine the committed
opcode reference came from. Run 33444039972, `mode=reference`.

```
gas unit: 1.6221 ns (1/1 of the 1.6221 ns baseline instruction)

syscall                        ns/call  gas now  gas proposed
GetBlockHeight                    71.9        0            48
GetCalldataLen                    79.2        0            48
GetCurrentAddress                 75.1        0            48
GetMsgSender                       79.0       0            48
GetSourceTransactionHash          75.8        0            48

constexpr static uint64_t kSyscallBaseCost = 48;
constexpr static uint64_t kSHA256HashBaseCost = 20;    // was 500
constexpr static uint64_t kSHA256HashByteCost = 11;    // was 100
constexpr static uint64_t kSHA256HashByteDivisor = 32; // new
```

That per-byte shape is what an earlier revision of the report proposed, and it
is not what was committed. See **What landed** below: pricing per compression
block says the same thing more accurately and removes the divisor entirely.

hog10 is roughly 40% faster than the container these numbers were first taken on
and far quieter with it: per-syscall standard deviations of 0.04-0.27 ns against
0.2-2.2. The *shape* is identical -- one crossing cost that swamps every
handler, SHA256 as a small base plus a sub-unit per-byte term -- which is what
makes the shape the finding and the container's absolute numbers superseded.

SHA256 there measures **111.4 ns + 0.5429 ns/byte** at R2 = 1.0000.

Three decisions are baked into the proposal, and each is arguable on its own.

**One shared base, not one constant per syscall.** The stateless reads land
within a few percent of each other, so what is being priced is the crossing and
not the handler; thirty constants would be thirty ways to say the same number.
`kSyscallBaseCost` is new, and adding it means adding an entry to
`kSyscallGasCostSchedule`.

**The base comes from the dearest measured syscall, and from the `vm` row.**
`vm` is what a guest actually causes -- crossing included -- and worst-case
pricing is the rule the opcode schedule already follows. One gas of it is
already charged as the `ecall` instruction, so the proposal subtracts it rather
than billing the crossing twice.

**SHA256 gets no base of its own.** The fit's intercept is ~111 ns, of which
about 79 ns is the crossing every syscall already pays and about 35 ns is the
one compression block SHA-256 performs on any input, padding included. What is
left is a couple of nanoseconds, and it is not a quantity this sweep measures --
see below.


### What landed

The schedule committed in `gas_cost_table.h`, measured on hog10 (run
33469060485) at a gas unit of 1.6241 ns:

```
constexpr static uint64_t kSyscallBaseCost         = 50;
constexpr static uint64_t kSHA256HashBlockCost     = 22;
constexpr static uint64_t kSHA256StreamCost        = 26;
constexpr static uint64_t kVerifySignatureBaseCost = 17750;
constexpr static uint64_t kVerifySignatureBlockCost = 131;
```

**Per compression block, not per byte.** SHA-256 works in fixed 64-byte blocks
and the measurement follows that: 34.53 ns per block, billing within 1.02x of
measured across the whole sweep. Pricing the real unit is also what removed the
divisor. A per-byte price is 0.33 gas, a fraction no integer
expresses, and the denominator that would fix it is precisely the sort of
billing-relevant constant that is easy to leave out of the digest -- the hazard
this document flagged before the schedule was written. In the block model there
is no denominator to forget: the 64 is SHA-256's, not a policy knob.

**No per-call base, and R2 is not why.** An earlier draft of this schedule
carried `kSHA256HashBaseCost = 2` and cited R2 = 1.000000 as the evidence for
the block model. The R2 does not support that reading. The sweep runs
{0, 32, 256, 1024, 4096, 16384, 32768} bytes, which is block counts
{1, 1, 5, 17, 65, 257, 513}: the 32768-byte point alone carries about two
thirds of the x-variance, and the y-variance is dominated by the same point. R2
prints 1.000000 there for a residual of roughly 4-5 ns RMS -- 0.03% at the top
of the sweep and about 4% at the 117 ns bottom, which is precisely where an
intercept is determined. Any roughly linear model scores ~1.0 on a sweep this
wide. The diagnostic that discriminates is the billed-to-measured ratio, which
is why the table below is the evidence and R2 is no longer quoted.

The base went with it, for a stronger reason than noise. `blocks(n)` is never
zero -- SHA-256 always compresses its padding, so even the empty input runs one
block, and no point in the sweep sits at the intercept. It is an extrapolation
to a block count no call can have. What it was estimating is what remains of a
117 ns call once the crossing (81 ns) and that first block (35 ns) are paid:
about 2 ns, inside the fit's own residual. `SHA256Finalize` writes the same
32-byte digest to guest memory and carries no base either, so a base was not a
rule the schedule followed in any case. The report still prints the residual as
a diagnostic -- if it ever grows past a few gas, the block model has stopped
describing the syscall.

**Touching a hash stream is priced, on both paths that do it.**
`SHA256Update` and `SHA256Copy` reach the frame's stream table, and both insert
into it when the destination slot does not exist yet -- an allocation with no
compression attached, so no block term bills it. A zero-byte reset update
creates a stream and hashes nothing at all. Neither charged for it.

Measured on hog10 by driving `HashBuffers` directly (run 33557627276), per
slot created, converged over occupancy: **Copy ~34.8 ns, Update ~41.0 ns**,
flat from occupancy 64 through 32768 -- an unordered_map insert stays amortized
O(1), so a chain that raises `max_sha256_buffers` does not change the price.
`kSHA256StreamCost` is the dearer of the two at that run's 1.6232 ns unit:
`ceil(41.0 / 1.6232)` = 26.

It is charged on every such call, not only the ones that allocate. Making the
price depend on whether the slot happened to exist would be the first cost in
this table that is a fact about the frame's state rather than about the call.
The flat charge overbills a repeat touch -- which measures near the crossing
alone -- by well under 2x, the direction the opcode schedule already rounds.

The syscall-path rows could not have found this. `max_sha256_buffers` is
network config, so the mock snapshot server the fixture talks to serves one
value and cannot sweep it; and `entry/SHA256Copy` repeats a single
(source, destination) pair, so the warm pass creates the destination and every
timed iteration after it takes the assignment branch.
`buffers/CopyFill` and `buffers/UpdateFill` drive the component instead, and
`entry/SHA256CopyNewSlot+Finalize` puts the insert back in the syscall-path
timed loop by erasing the destination each iteration.

**No per-call base, and R2 is not why.** An earlier draft of this schedule
carried `kSHA256HashBaseCost = 2` and cited R2 = 1.000000 as the evidence for
the block model. The R2 does not support that reading. The sweep runs
{0, 32, 256, 1024, 4096, 16384, 32768} bytes, which is block counts
{1, 1, 5, 17, 65, 257, 513}: the 32768-byte point alone carries about two
thirds of the x-variance, and the y-variance is dominated by the same point. R2
prints 1.000000 there for a residual of roughly 4-5 ns RMS -- 0.03% at the top
of the sweep and about 4% at the 117 ns bottom, which is precisely where an
intercept is determined. Any roughly linear model scores ~1.0 on a sweep this
wide. The diagnostic that discriminates is the billed-to-measured ratio, which
is why the table below is the evidence and R2 is no longer quoted.

The base went with it, for a stronger reason than noise. `blocks(n)` is never
zero -- SHA-256 always compresses its padding, so even the empty input runs one
block, and no point in the sweep sits at the intercept. It is an extrapolation
to a block count no call can have. What it was estimating is what remains of a
117 ns call once the crossing (81 ns) and that first block (35 ns) are paid:
about 2 ns, inside the fit's own residual. `SHA256Finalize` writes the same
32-byte digest to guest memory and carries no base either, so a base was not a
rule the schedule followed in any case. The report still prints the residual as
a diagnostic -- if it ever grows past a few gas, the block model has stopped
describing the syscall.

Every constant the cost functions read is an entry in
`kSyscallGasCostSchedule`, so all of them are hashed. Block *geometry* -- the
64, the 9-byte padding tail -- is static_asserted rather than hashed, because a
wrong value there is a bug in the cost model rather than a repricing of it.

How it bills against what was measured, crossing included:

| row | measured ns | charged | ratio |
|---|---|---|---|
| `SHA256/0` | 117.4 | 73 gas | 1.01x |
| `SHA256/1024` | 673.2 | 425 gas | 1.03x |
| `SHA256/32768` | 17798.4 | 11337 gas | 1.03x |
| `SHA256Update/4096` | 2286.0 | 1485 gas | 1.06x |
| `SHA256Finalize` | 114.0 | 73 gas | 1.04x |
| `VerifySignature/0` | 28635.5 | 17932 gas | 1.02x |
| `VerifySignature/1024` | 30631.7 | 18980 gas | 1.01x |
| `GetBlockHeight` | 69.9 | 51 gas | 1.18x |

Nothing undercharges, and the spread across a single syscall's argument range
is 1.02x for SHA256 where the old schedule's was 43x.

**Three things were free and are not any more.** `VerifySignature` did ~17.6k
gas of real work for nothing. The incremental hash family -- `SHA256Update`,
`SHA256Copy`, `SHA256Finalize` -- reached the same compression function as the
one-shot at zero cost, so `SHA256Update(4096)` did the work of a
410,100-gas `SHA256(4096)` for free. And the entry path itself was free however
often it was crossed, which is what `kSyscallBaseCost` fixes; it is charged
centrally in `wrap_after_parse_async` so a syscall added later cannot forget it
and a call that faulted still pays for the entry it used.

**A failed signature is charged.** `syscall_VerifySignature_inner` returned zero
gas when verification failed, but ed25519 only exits early when the encoded
point fails to decompress -- which the attacker choosing the signature
controls. Billing failure at zero would have sold unbounded verification for
the price of the crossing.

### The per-byte term cannot be an integer, and that is fixable cheaply

On hog10 SHA256's per-byte cost is **0.5429 ns** against a 1.6221 ns gas unit --
**0.335 gas per byte**, and no integer expresses it. A flat 1 gas/byte
overcharges by **3.0x**.

Two ways out, same precision, very different prices.

**Raise the resolution.** `--resolution 4` makes the unit 0.41 ns. It also
multiplies every number in the opcode schedule, the syscall schedule, and the
block gas limits by four, because the two tables cannot be denominated
differently.

**Or split the charge over several bytes.** `ceil(bytes * 11 / 32)` is 1.03x, at
resolution 1, changing nothing outside `SHA256HashCost`. The report prints both
so the comparison is visible:

```
per byte that is 0.3347 gas, which no integer can express:
  flat 1 gas/byte overcharges by 2.99x
  11 gas per 32 bytes overcharges by 1.03x
```

What it buys, across the size sweep (ns of real work per gas, against 1.62 for a
dispatched instruction):

| bytes | now | flat 1/byte | 11 per 32 bytes |
|---:|---:|---:|---:|
| 0 | 0.267 | 1.962 | 1.962 |
| 256 | 0.011 | 0.844 | 1.753 |
| 4096 | 0.006 | 0.565 | 1.594 |
| 32768 | 0.006 | 0.546 | 1.581 |
| **spread** | **48.8x** | **3.6x** | **1.24x** |

Spread is worst-over-best: how differently the same syscall is billed depending
on an argument the caller picks. Today it is 49x. A flat integer per-byte cost
takes that to 3.6x. The divisor takes it to 1.24x, and the residual is the fixed
base amortizing over a growing input, which no per-byte term can remove.

A note on the fraction itself. On a slower host the rate was 0.457 gas/byte and
the answer was a tidy `1 gas per 2 bytes`; on hog10 it is 0.335 and the answer
is `11/32`. The pretty fraction was an artifact of that host's ratio, not a
property of SHA256 -- worth knowing before anyone gets attached to one. The
search also only tries power-of-two denominators, which is why it lands on 11/32
rather than something nearer 1/3; widening it would find shorter fractions, at
the cost of a divisor that is not a shift.

**One thing to check before adopting a divisor.** `gas_schedule_digest()` in
`artifact.h` hashes `kSyscallGasCostSchedule`'s names and costs -- not
`SHA256HashCost`'s formula. A divisor written only as a constant inside the
lambda would change what every contract is billed while the digest sat still,
which is precisely the failure the digest exists to prevent. It has to be an
entry in the schedule array to be hashed. The report says so in its output.

### How much headroom the base has, and why that is a question

The base is worst-case-measured -- the dearest of the stateless reads -- and its
only headroom is the rounding up to the next whole gas. At this unit that is
about **1%**:

```
One shared base of 48 gas, taken from GetCalldataLen at 79.2 ns.
That buys 79.5 ns of budget: +0.3% over the dearest measured call.
```

Three tenths of a percent. That is not a margin, it is where the rounding
happened to land, and on a different night it lands somewhere else.
`--margin PERCENT` applies one before rounding, and the report says what the
result buys:

```
--margin 0  ->  kSyscallBaseCost = 48   (+0.3% over the dearest measured call)
--margin 5  ->  kSyscallBaseCost = 51   (+6.5%)
```

The calibration workflow prints both, so a reviewer sees the choice rather than
inheriting it.

How much margin a protocol constant deserves is a policy call, not a
measurement. The report's job is to make the size of it visible rather than let
it be discovered later; picking it is yours.

### Storage: the NNInt rows

The first stateful family, measured in the same binary. NNInt is the smallest
storage syscall -- a 16-byte value, no length axis -- so what it isolates is
the machinery every storage syscall shares: the item view, the frame stack, the
snapshot cache. Bytestrings add a byte term on top of this and are the next
row set.

**Why a storage syscall cannot be priced from its arguments.** The stateless
rows above cost what they cost whatever came before them. A storage read does
not. `SystemCallHandler::get_nnint` asks `InvocationFrameStack::get_item_view`
for the key, and what that costs depends on three things the arguments do not
name:

- whether this transaction has touched the key already -- the view is in the
  active frame, and the call is one map lookup;
- if not, how deep the invocation stack is -- a miss in the active frame looks
  in *every* parent frame before creating a fresh view, so a first touch at
  depth 200 does 200 lookups where the same call at depth 1 does one, and it
  does them whether or not any parent holds the key;
- and, for a forced load, whether the block's snapshot cache holds the key --
  a hit is a shared-lock find, a miss is a database round trip.

Writes have the mirror shape: `set_nnint` stages an item in the frame's view
and consults nothing, and the work lands later, when a successful child frame
moves its staged items into its parent (`commit_successful_child_frame`) and
again when the root commits its delta batch to the database.

So each of those is a regime, and each regime is a row that puts the handler in
exactly that state before every timed call:

| row | what every timed call does |
|---|---|
| `entry/NNIntGet`, `vm/NNIntGet` | warm read: the view is in the active frame, loaded from the snapshot |
| `entry/NNIntGetNoLoad` | warm read without `force_load_from_snapshot` |
| `entry/NNIntSetAdd`, `vm/NNIntSetAdd` | warm write: overwrites a staged item already in the frame |
| `frame/Push/D` | open D frames and discard them; no syscalls. The baseline under every sweep |
| `frame/NNIntGetFirstTouch/D/K` | in a fresh frame at depth D, read K distinct keys once each: miss the frame, walk D+1 parents, create a view, load it from the snapshot cache |
| `frame/NNIntGetFirstTouchNoLoad/1/K` | the same with no load: create the view and answer from it |
| `frame/NNIntSetAddFirstTouch/1/K` | first-touch writes: create the view, stage the item |
| `frame/NNIntSetAddMerge/K`, `frame/NNIntSetAddDiscard/K` | K writes in a child frame that then commits into a fresh parent, or is discarded. The difference is the per-item commit cost |
| `cold/NNIntGet` | a read that misses the snapshot cache, once per call |

**How each regime is kept to itself.** The sweep rows open their frames inside
the timed iteration and close them as *failed*, so whatever they touched is
discarded and the next iteration starts from the state the last one found. Each
regime reads and writes its own key space (the key's first byte is the regime
tag), so one row's warm pass cannot leave a key in a frame, or in the snapshot
cache, that another row then finds warm. The first-touch keys are seeded in the
mock snapshot and faulted into the cache by the row's warm pass, so the timed
reads are cache hits by construction; the cold row's keys are used strictly
once, from a pool sized against a fixed iteration count, and the row fails with
a message rather than recycling one. Every call in every iteration is checked,
not only the warm pass: a stateful row can start failing partway, and one that
timed the error path from then on would report a fast syscall.

The cold row is registered last, because it leaves the shared snapshot cache a
quarter-million entries larger than every row before it found it, and a lookup
into that cache is not the same lookup.

**How to read the sweeps.** Each `frame/` family is a line in K per depth, fit
through the measured K=0 point (`frame/Push/D`, the same frames with no calls).
The slope is the per-key cost and the report prints it. Two checks go with it,
because R2 on a sweep this wide says nothing (the 4096 point carries almost all
the variance): the fitted intercept has to agree with the push baseline it
extrapolates to, and the per-point residuals have to stay flat in K. A residual
that grows with K is a per-key cost that is not constant -- the frame's map not
being O(1), or something else in the iteration scaling with the keys -- and the
report flags an intercept more than 25% off.

The depth axis is the parent walk. The sweep runs at depths 1, 8 and 16 --
8 and 16 being where a chain is expected to set `max_cross_invoke_depth` --
and at the repo's stock maximum of 254, which no chain is expected to run at
and is there as the far point that keeps the per-frame cost resolvable now that
#3771 has taken it under a nanosecond. The per-key cost at the deepest depth
minus the shallowest, over the difference, is what one open parent frame adds
to every first touch.

**What the cold row is, and is not.** It measures everything on the executor's
side of a snapshot-cache miss: the miss under the shared lock, the
`DatabaseProxy` RPC -- serialized, over a local socket -- the
`SnapshotStorageItem` built from the reply, the cache insert. The server
answering is the benchmark's in-process mock, whose lookup is a hash-map find.
So it is the fixed cost of *going* to the database and not the cost of the
database, a floor under a cold read rather than the read. The report prints it
in its own table and keeps it out of `--schedule`.

There is a more basic reason it is not proposed. Whether a key is in the
snapshot cache depends on which transactions ran before this one in the block,
and validators schedule a block's transactions differently. A price that
depended on it would not be deterministic. Whether this transaction has touched
the key before depends only on this transaction's own earlier syscalls, which
every validator replays identically -- so a per-transaction first-touch charge
is a legitimate term and a per-block cache-miss charge is not. That is the
distinction the rows are built around, and it is the same one the EVM arrived at
with warm and cold access lists.

**Results.** Measured on the bench pool -- hog10, AMD EPYC 9554, the machine
the committed opcode reference comes from -- in run 33725560215, engine
`d4697729c27d6dbd`, at a gas unit of 1.6217 ns. Five repetitions, means.
These are post-#3771 and post-#3814; see "What #3771 bought" and "What #3814
bought" below for what the two key changes moved, and the #3557 note for a
frame cost that moved under them.

Every row is an `entry/`-level measurement and the ratio is against the
stateless `entry/GetBlockHeight` on the same run (55.0 ns), so the crossing is
in both sides and cancels. The one `vm/` row is marked and takes the `vm/`
baseline (74.3 ns) instead.

| regime | ns | x a warm `GetBlockHeight` |
|---|---:|---:|
| `entry/NNIntGet`, warm | 158.4 | 2.9 |
| `entry/NNIntGetNoLoad`, warm | 156.2 | 2.8 |
| `entry/NNIntSetAdd`, warm | 170.6 | 3.1 |
| `vm/NNIntGet`, warm, crossing included (vs `vm/`) | 182.6 | 2.4 |
| one frame opened and discarded | 124.6 | 2.3 |
| first touch, depth 1, 1 key in the frame | 257.1 | 4.7 |
| first touch, depth 1, 4096 keys in the frame | 303.7 | 5.5 |
| first touch, depth 16, 4096 keys | 313.7 | 5.7 |
| first touch, no load, depth 1, 4096 keys | 216.5 | 3.9 |
| first-touch write, depth 1, 4096 keys | 227.1 | 4.1 |
| each parent frame, per first touch | 0.9 | 0.02 |
| committing one staged item into a parent, 4096 items | 51.5 | 0.9 |
| snapshot-cache miss, against the mock | 42174 | 764 |

Four things follow.

**A warm read is about three stateless reads, and the difference is the key
hash.** `NNIntGet` at 158.4 ns against 55.2 ns, with `NNIntGetNoLoad` 2 ns
under it. What the extra buys is `get_item_view`'s lookup: hash the key, probe
the frame's map, read the variant. A warm write costs the same to within
1 ns, since it is the same lookup plus a variant assignment.

**A first touch is not one cost; it depends on how many keys the frame already
holds.** At depth 1 the first of 4096 first touches costs 257 ns and the
4096th 304, and carrying the sweep out to 262144 keys reaches 1017 -- a 4.4x
spread the report marks `NOT FLAT`. The frame's `modified_items_` is an
`unordered_map`: as it fills it rehashes, allocates a node per key, and stops
fitting in cache. Subtracting the warm call leaves 858 ns as the first touch
proper at the top of the sweep, of which the snapshot-cache lookup is most --
the no-load sweep's surcharge there is 230 ns and the write's 218 ns, and both
climb far more gently in K (2.06x against the loaded sweep's 4.36x) because
neither consults the cache.

That climb is not the map rehashing. `unordered_map` rehashes at 1, 2, 4, 8...
entries, so the total rehash work across N inserts is O(N) and the amortized
per-insert cost is a constant -- rehashing alone gives a flat curve, not a
rising one. What rises is the working set against the cache hierarchy, and the
sizes line up with it: at ~190 bytes an entry the frame's map is ~780 KB at
4096 keys (still L2), 3.1 MB at 16384, 12.5 MB at 65536, and ~50 MB at 262144,
past the 32 MB of L3 one thread gets on a single EPYC CCD. Each insert also
chases a node pointer, since libstdc++ keeps a separately allocated node per
entry, so once the map is out of cache a first touch pays two dependent misses
rather than one. The lever is locality rather than `reserve()`: a flat,
open-addressed map keyed by the pre-hashed `FrameItemKey` would drop the
pointer chase and shrink the footprint.
A fitted line does not describe the loaded sweep: an early draft of the report
fitted one and got a negative intercept, which is why the report prints the
per-key cost at every K and takes the dearest.

**The parent walk is linear and small.** Each open parent frame adds **0.9 ns**
to a first touch: at K=4096, depth 1 costs 304 ns, depth 8 308, depth 16 314,
and the stock maximum of 254 frames 535. That is what a loop of hot map probes
should cost, and it is what the code does -- a miss in the active frame walks
ancestors calling `find` on each, with the hash already cached since #3771.

An earlier version of this section reported a ~280 ns step between depth 1 and
depth 8 and could not explain it. It was a benchmark ordering bug, not a
property of the walk. The loaded rows are the only ones that fault keys into
the block's snapshot cache, that cache is shared by every row and never reset,
and depth 1 was the depth that ran the long key sweep -- so by the time depths
8, 16 and 254 ran, they were probing a cache holding a quarter-million entries
against the handful depth 1 had seen. Running the depth axis over the short
sweep first, and the long sweep after it, puts every depth at the same cache
state and the step disappears. It reproduced both ways on one host before the
fix landed, which is what identified it: filtering the long rows out of the run
removed the step, and putting them back restored it.

The lesson generalises past this row. A shared, monotonically growing structure
turns registration order into a hidden variable, and it does not fail loudly --
it reads as a clean result for whichever axis is confounded with it. The commit
rows had the same bug earlier for the same reason.

The walk is paid whether or not any parent holds the key, so a
chain that raises `max_cross_invoke_depth` far enough will see it again, but at
8 or 16 it is now noise. It cannot be dropped outright, either: the reentrance
ban stops one contract being live twice on the stack at once, but a contract
invoked twice in sequence by the same caller has its first frame's views merged
into the caller's at commit and its second frame has to find them there, and
the native-token and auth paths read keys under addresses other than the active
frame's. The depth-254 row's K=1 cell is the one place
this is not resolvable: isolating a single ~260 ns touch by subtracting a
30 µs frames-only baseline is below what five repetitions measure, and the
K=4096 cell (535 ns) is the one to read.

**Writes are cheap to stage and paid for later.** A first-touch write costs
what a no-load first touch costs; the syscall stages an item and stops.
Committing that item from a successful child into a fresh parent costs another
32-52 ns per item, and the root commit's delta batch to hog_db is not measured
here at all. Any per-write price has to prepay both halves, because neither is
a syscall the schedule sees.

The cold row is what it says: 42.2 µs against the in-process mock, 266x a warm
`NNIntGet`, most of it the coroutine round trip through the proxy rather than any
lookup. It is on the record so the size of what the schedule declines to price
is known, not as a candidate for the table.

**What the run proposes, and what to take.** The report prints

```
kStorageFirstTouchCost = 532;  // surcharge over a warm NNIntGet (158.4 ns)
kStorageCommitItemCost = 32;   // per staged item a child hands its parent
```

because it takes the dearest point of the sweep, 262144 keys in one frame,
which is what a schedule must charge if it means to cover the whole range. The
surcharge measured at each K is

| keys in the frame | 1 | 16 | 256 | 4096 | 16384 | 65536 | 262144 |
|---|---:|---:|---:|---:|---:|---:|---:|
| surcharge, gas | 61 | 46 | 57 | 90 | 125 | 235 | 529 |

so the two ends differ ninefold and the report's number is not the one to
adopt. **The settled figure is 100.** It covers a first touch costing up to
321 ns in total, which is every frame up to roughly ten thousand keys; it is
twice `NNIntGet`'s own 50, so it reads as a round multiple rather than a
false-precision measurement; and it covers a first-touch *write* comfortably,
since a write never consults the snapshot cache and its surcharge at 4096 keys
is 62 gas. One constant therefore serves reads and writes both.

What 100 declines to cover is the tail past ~10k keys in a single frame, and
what bounds that is the gas a transaction must burn to get there: at 100 gas of
surcharge on top of `NNIntGet`'s 100, four thousand first touches is about
800K gas and a quarter-million is about 49M. If the per-transaction or
per-block limit sits under that, the tail is unreachable and the gas budget is
already the cap.

A key-count cap would need care if one is added: `get_item_view` creates a view
in `modified_items_` on a **read** miss as well as a write, so a limit on keys
*modified* would not bound the map -- a read-only transaction can still walk it
into the tail. The bound has to be on keys *touched*.

Adopting 100 is a gas-schedule change and not a benchmark change: the constant
is folded into `rv32::kGasScheduleDigest` and pinned in committed artifacts, so
it lands as a coordinated update rather than with this measurement.

These are charges *on top of* the syscall table, which prices `NNIntGet` at
100 gas (the 50-gas shared crossing base plus 50 of its own). Nothing is
adopted here: the schedule is unchanged, and these are the input to that
decision.

**How far does the key count scale.** Nothing caps the keys a transaction may
touch except its gas budget, so a price read at 4096 is only sound if the cost
has stopped moving by then. It has not. Depth 1 on the development container,
ns per first touch -- shape only, see the caveat below:

| K | container, no-load | container, loaded | hog10, no-load | hog10, loaded |
|---:|---:|---:|---:|---:|
| 1 | 293 | 504 | 219 | 257 |
| 256 | 342 | 486 | 203 | 251 |
| 4096 | 378 | 667 | 217 | 304 |
| 16384 | 449 | 1281 | 244 | 361 |
| 65536 | 723 | 2226 | 271 | 539 |
| 262144 | 1266 | 2783 | 386 | 1017 |

The hog10 columns are post-#3814 (run 33725560215); the container columns
predate it and are kept for the shape of the knee, which #3814 does not move.


The container bends early and hard: flat to about 4096, then roughly 1.6-1.75x
per 4x keys. hog10 stays flat much further -- 174 ns at one key and 187 at
4096, still only 243 at 65536 -- which is its 256 MB of L3 absorbing what the
container's 33 MB could not. That is the shape a map outgrowing each cache
level has, and it says the knee is a property of the host rather than of the
code. At 262144 keys the frame's map alone is ~50 MB.

Read the *surcharge* rather than the total when judging how much this matters
for a price, because the warm call is a fixed component that flattens the
ratio. On hog10 the loaded first touch costs 145 ns over its warm call at
4096 and 858 ns at 262144 -- 5.9x, where the totals differ by 3.3x.

So pricing at the top of the sweep is not attractive: it would charge an
ordinary transaction touching a handful of keys 3-4x what it costs. Pricing at
the knee instead leaves a transaction touching hundreds of thousands of keys
underpaying, and what bounds that is the gas budget it has to burn getting
there -- at the 200 gas a first touch costs under the settled constant (the
syscall's 100 plus a 100 surcharge), 262144 of them is about 52M gas before the
contract does anything else. Which point to price at is a policy call about
that budget rather than something the measurement settles.

**Two caveats on the table above.** The knee's location is a property of the
host's cache, which is why both hosts are shown rather than the pool alone:
the container is what a small-cache validator would see. And the loaded
columns are an upper
bound: its keys have to exist in the benchmark's mock, and at the top of the
sweep that map is ~90 MB resident competing for the same cache. That is
scaffolding rather than a cost a contract causes, which is why the no-load
column -- which seeds nothing and consults neither the mock nor the cache --
is the one to read for how the frame's own map scales. The rows that need no
seeded snapshot run first for that reason, and the commit rows run before the
seeded sweep too: they are a difference of two whole-iteration timings, and
measured after it they read 37-255 ns per item against 35-56 for the same
rows before the sweep existed.

**The parent walk is measured and deliberately not priced.** At 0.9 ns per
open frame it rounds to 1 gas, which is below what this benchmark resolves --
and a constant in this table is not free, since every one is folded into
`rv32::kGasScheduleDigest`, so adding one means a coordinated gas-schedule
update for a term that is noise. What that leaves unpriced is bounded by what
it costs to get deep in the first place: at the stock maximum of 254 frames it
is about 140 gas per first touch, and reaching depth 254 means 254 nested
invokes, each of them dearer than the walk it adds. At the 8 or 16 a chain is
expected to configure it is under 10 gas. The sweep keeps measuring it, so a
regression -- or a chain setting `max_cross_invoke_depth` far higher -- shows
up as a rising per-frame number rather than as silence.

**What #3771 bought.** The frame maps were keyed by the XDR `AddressAndKey`,
so every probe serialized it into a buffer and SipHashed 68 bytes, and a first
touch at depth D did that D+1 times. `FrameItemKey` lays the address, key type
and payload out flat, hashes them once when the key enters the frame stack, and
every frame probes with the cached hash. Measured before and after on the
development container (the only host that ran both), which is why these are
container nanoseconds and the table above is hog10's:

| regime | before ns | after ns | change |
|---|---:|---:|---:|
| `entry/NNIntGet`, warm | 211 | 198 | -6% |
| first touch, depth 1, 4096 keys | 1007 | 669 | -34% |
| each parent frame, per first touch | 51 | 1.0 | -98% |
| first touch at depth 16, 4096 keys | 2018 | 654 | -68% |
| committing one staged item, 256 items | 222 | 95 | -57% |

The depth term went because the walk stopped re-hashing; what remains per frame
is an integer compare and a bucket probe. hog10 puts that residue at about
1 ns per frame beyond depth 8, though see the parent-walk paragraph above for
the step below that depth, which this does not explain.
What did not change is the shape in K, which is the map growing rather than
anything about the key, and is the next thing to take out of the structure
rather than price around.

**What #3814 bought.** #3771 took the double hashing out of the *frame* maps
but left it in the snapshot cache, which was still keyed by the XDR
`AddressAndKey` and so serialized and SipHashed the same 68 bytes again on
every loaded lookup. #3814 keys it by the pre-hashed `FrameItemKey` too, so a
loaded first touch hashes its key once for the whole path.

The way to read that is the gap between the loaded and no-load sweeps, since
only the loaded path consults the cache. Depth 1 on hog10, ns per first touch,
run 33659646711 against run 33725560215:

| K | gap before | gap after | change |
|---:|---:|---:|---:|
| 1 | 82 | 38 | -54% |
| 256 | 90 | 48 | -47% |
| 4096 | 142 | 87 | -39% |
| 16384 | 182 | 117 | -36% |
| 65536 | 386 | 268 | -31% |
| 262144 | 766 | 630 | -18% |

Half the gap at small K was the second hash, which is what the change targeted
and what it removed. The tail is a different matter: the gap still triples from
4096 to 262144, and the loaded sweep is still 2.6x the no-load one at the top
(it was 3.2x). So the steep tail is not the cache re-hashing serialized keys --
it is cache pressure from two maps of hundreds of thousands of entries, and
there is no key-layout change left that takes it out.

**#3557 roughly doubled the cost of opening and closing a frame, and the sweeps
carry it.** `finish_frame()` became a coroutine when the hogdb client went
async, and a push/pop pair went from ~60 ns to ~118 ns per frame:

| depth | before ns/frame | after ns/frame |
|---:|---:|---:|
| 1 | 69.3 | 125.5 |
| 8 | 60.7 | 118.4 |
| 16 | 59.5 | 117.1 |
| 254 | 61.3 | 119.2 |

Run-to-run CV is under 0.25% on both sides at every depth, so this is not
noise. It is measured here rather than diagnosed: the coroutine frame
`finish_frame_impl` allocates on every call is the obvious candidate, and
whether that is avoidable on the arm that never suspends is a question for
whoever owns that path. It matters for pricing because every cross-contract
invoke pays it, and because it raises the `frame/Push/D` baseline the storage
sweeps subtract -- the warm syscall rows moved with it too, 8-19% dearer,
while `entry/GetBlockHeight`, which touches neither frames nor storage, did not
move at all (56.1 -> 55.0 ns).

A note on pinning: `taskset -c <one core>` crashes this binary with SIGFPE in
`x23::PerCpuSet::lookup` before the first row, because the per-CPU set sizes
itself from the affinity mask and a single-core mask leaves it dividing by
zero. A mask of several cores does not trip it. These numbers are unpinned.

### What follows

- **The zero-gas reads are the urgent item.** On hog10 a loop of
  `GetBlockHeight` burns ~72 ns per iteration and is charged for the handful of
  instructions that set the call up -- roughly 5 gas against 72 ns of real work,
  an order of magnitude worse per gas than the worst opcode the calibration
  found.
- **`SHA256HashCost` needs re-deriving, not scaling.** On hog10 the measured
  base-to-per-byte ratio is about 205:1 (111 ns : 0.54 ns); the charged ratio is
  5:1 (500 : 100). No single multiplier fixes both, and the per-byte term needs
  a divisor to be expressible at all.
- Take the numbers from the bench pool, not from a container, and from more than
  one run -- the same rules the opcode schedule is under. Absolute nanoseconds
  are host-specific; the ordering and the ratios are the finding.
- Both are gas-schedule changes: `kSyscallGasCostSchedule` is folded into
  `rv32::kGasScheduleDigest`, so they move with the pinned digest in
  `artifact.h` and every committed artifact, exactly as the opcode schedule did.
- Syscall costs are measured on every workflow run but **not gated**. There is
  no committed reference for them yet, and inventing one from a single run of a
  new benchmark would be a gate that describes nothing. The JSON is filed from
  the first run, so the history is there by the time there is something to
  compare against.
- The engine fingerprint should cover the syscall path. It hashes the
  instruction schedule, the structural limits and the fusion matrix;
  `kSyscallGasCostSchedule` is already in `kGasScheduleDigest` so the input
  exists, and `HOGD_RV32_ENGINE_SOURCES` wants a syscall-path equivalent, or a
  rewrite of `syscall_entry_outer.h` moves these numbers with nothing saying so.
- NNInt is measured (above) and not yet priced. The rest of the storage family
  is not: bytestrings add a length axis to the same machinery and are the next
  rows; `DeleteObject` and the retrocausal queue calls follow the same
  frame-and-snapshot pattern with their own item types; the auth registry calls
  reach a different table. What none of the rows reach is the root commit --
  `try_commit_transaction`'s delta batch and hog_db's item locks -- which is
  where a write's cost lands last, and needs a real database rather than the
  mock to measure.
- The snapshot-cache miss is measured against the mock and deliberately not
  proposed. Its worst case is a hog_db read, which is a database cost rather
  than a syscall cost and is not reproducible the way these are. Even the
  stateless set has a cold path -- the first `GetBlockHeight` in a block
  fetches the db tip through the proxy and every later one reads an atomic. The
  numbers above are the steady state, which is what a contract making more than
  one syscall sees.
