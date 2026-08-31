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
```

`--schedule` refuses without `--gas`, and refuses a `--gas` file whose engine
fingerprint or host does not match the benchmark's. That is not ceremony. Gas is
one currency: a syscall's price has to be in the same unit as an instruction's,
that unit is the baseline instruction the opcode calibration measured, and
joining two hosts' numbers produces a complete, plausible schedule that is wrong
by whatever the two machines differ by. The check is the only thing standing
between a careful measurement and a confidently mispriced one.

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

**SHA256's base is its excess over the crossing, not its total.** The 111.4 ns
is the fit's intercept over the whole size sweep, not the 115.9 ns measured at
zero bytes -- different quantities, agreeing to 4%. Of it, about 79 ns is the
crossing every syscall already pays, leaving 32.2 ns of SHA256's own work.
Charging its full base as well would bill the crossing twice.

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
- Stateful syscalls remain unmeasured and are the harder half: their worst case
  is a snapshot-cache miss, which is a DB cost rather than a syscall cost and is
  not reproducible the way these are. Even the stateless set has a cold path --
  the first `GetBlockHeight` in a block fetches the db tip through the proxy and
  every later one reads an atomic. The numbers above are the steady state, which
  is what a contract making more than one syscall sees.
