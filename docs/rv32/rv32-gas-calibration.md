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
scripts/rv32_gas_calibrate.sh --out /tmp/gas-1.json
scripts/rv32_gas_calibrate.sh --out /tmp/gas-2.json
scripts/rv32_gas_report.py /tmp/gas-1.json /tmp/gas-2.json            # what moved
scripts/rv32_gas_report.py /tmp/gas-1.json /tmp/gas-2.json --schedule # merged
scripts/rv32_gas_report.py /tmp/gas-1.json --workloads
```

The script builds the `rv32_gas_calibrate` target through the project's
ordinary Release build and reuses an existing Release build directory when it
finds one. Only the RV32 ISA core is linked, so the target's own build is small.
A full run takes a few minutes.

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
codegen the node runs. Per opcode, the harness measures the marginal cost of
one more instance of that opcode in a body of otherwise identical filler, in
several operand contexts, and reports the worst.

The contexts are the load-bearing part. An opcode does not have one cost, and
which end of its range applies is the guest author's choice, so gas has to
price the end an adversary can force.

## First results

Host: 4-core Intel Xeon @ 2.80GHz, 32 KiB L1d and 1 MiB L2 per core, Linux,
Release build (`-O3 -march=native`, LTO). Development container, not a
validator; treat the ordering as the finding and the absolute nanoseconds as
host-specific.

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

`scripts/rv32_gas_check.sh` runs a calibration and compares it against
`hogd/hog_exec/runtime/rv32/gas_calibration/reference.txt`, exiting non-zero if
the calibration no longer describes the engine. It is meant for a periodic job
-- nightly, weekly, whatever cadence suits -- on a reasonably quiet machine, and
it is not built to run on every change.

It checks the engine fingerprint (deterministic, cannot flake) and a set of
guarded costs expressed as ratios to the baseline instruction measured in the
same run, so the reference works across hardware. The tolerance is loose by
design: only cells at or above 1.5x baseline are guarded, and a guarded cost has
to double before it fires. That separates a real blowout from the ordinary
few-percent drift of a shared machine, and keeps the job worth reading.

`--fingerprint-only` answers "did the engine change?" instantly, without
measuring, for use by hand after touching the interpreter.

## Re-running after an interpreter change

Every calibration JSON records an engine fingerprint: a hash of the gas
schedule, the engine's structural limits, and which instruction pairs the
lowering fuses, probed from the engine itself rather than hashed from source
text. It is stable across compilers, optimization levels, and hosts, and moves
when the lowering's behaviour moves -- disabling a single fusion rule changes
it.

That makes the check after a `threaded_machine.h` change mechanical:

```sh
scripts/rv32_gas_calibrate.sh --out /tmp/gas-after.json
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

## Before changing the schedule

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
- Price `JAL`, `JALR`, `FENCE`, and the trapping opcodes. The substitution
  design cannot hold them constant, so they are unmeasured and would keep
  placeholder costs.
- Re-run after interpreter changes, not only before schedule changes. A new
  fusion rule or a dispatch change moves these numbers, and a schedule
  calibrated against the old engine silently loses its margin. The engine
  fingerprint is there to make that detectable rather than remembered.

## Syscalls

Not yet measured, and the bigger half of the problem: `gas_cost_table.h` prices
one syscall family (SHA256) and returns zero direct host gas for the rest.

The sweep machinery generalizes without redesign. For a syscall, the
independent variable becomes the number of calls at a fixed argument size,
whose slope is the per-call base cost, and then the argument size at a fixed
call count, whose slope is the per-byte cost -- which is the `base + per-byte`
shape `SHA256HashCost` already has. What has to be built is the kernel shape
(a body of `ecall`s against a host that performs the real syscall) and a host
that is the production syscall path rather than the halting stub the opcode
harness uses.
