# RV32 gas calibration

The instruction harness measures `ThreadedMachine` with tracing disabled. The
separate syscall benchmark measures the production `syscall_entry` path. Both
record host, compiler, build flags, revision, and engine provenance in their JSON.
These measurements inform the schedule; they do not establish a universal
execution-time bound.

## Run and report

Run from the repository root with an explicit Release build directory. The
scripts configure a new directory or reuse an existing Release tree; they refuse
to reconfigure a non-Release tree. The binaries reject non-Release builds unless
`--allow-nonproduction` is supplied for harness development.

```sh
B=build-release-ccache-tmp
scripts/rv32_gas_calibrate.sh "$B" --out /tmp/gas.json
scripts/rv32_gas_calibrate.sh "$B" -- --quick
scripts/rv32_gas_calibrate.sh "$B" --out /tmp/jalr.json -- --op=JALR --no-syscalls
scripts/rv32_gas_calibrate.sh "$B" -- --syscalls-only

scripts/rv32_gas_report.py /tmp/gas.json
scripts/rv32_gas_report.py /tmp/gas.json --schedule
scripts/rv32_gas_report.py /tmp/gas.json --syscalls
scripts/rv32_gas_report.py /tmp/gas.json --workloads
scripts/rv32_gas_report.py /tmp/gas.json --predict
scripts/rv32_gas_report.py before.json after.json
scripts/rv32_gas_report.py run1.json run2.json --schedule

scripts/rv32_syscall_bench.sh "$B" --out /tmp/sys.json
scripts/rv32_syscall_report.py /tmp/sys.json --gas /tmp/gas.json --schedule
```

Use full, repeated runs for schedule proposals. Filtered runs are useful for
investigating a case, but do not provide the common normalizer or full coverage.
Syscall price proposals require a gas calibration from the same host and engine.

## Instruction measurements

For each opcode and operand context, the primary estimate is the time of a
saturated loop body divided by its target count (`ns_saturated`). Single-opcode
bodies disable fusion; pair measurements retain it, including the `auipc;jalr`
case whose audit requires fusion. The resulting prices do not assume that a
guest's instructions will fuse.

The harness also varies target count in a fixed body of filler instructions.
That substitution sweep reports a slope and its standard error as diagnostics.
It is not the primary estimator: layout differences between kernels can dominate
the timing difference and produce negative slopes. `cost_ns()` in
`scripts/rv32_gas_report.py` prefers saturation, then an absolute upper bound,
then the older substitution estimate. `saturated_note` explains missing
saturation. A slope's uncertainty applies to the fit, not to the saturated cost.

Contexts cover dependent and independent operands, branch direction, memory
working sets, and division operand magnitude. The reporter selects the most
expensive measured context per opcode. Timing points are interleaved and reduced
by median; the lowering audit checks that the generated body retains its
intended handlers and run boundaries.

`JAL` and `JALR` use dedicated control-flow graphs with sequential, reversed, and
seeded shuffled traversals over dense and spaced layouts. JAL includes link and
no-link cases. JALR includes fixed-base controls and a chain where each target
uses the previous jump's link register. Unvisited padding traps. The 12-bit
immediate limits the pure JALR chain's footprint.

The JALR `target_chase` case repeatedly dispatches from one PC through a linked
table. Its absolute time includes two dependent loads and a returning JAL per
dispatch. The report excludes it from individual opcode prices and checks it
against `2 * gas(LW) + gas(JALR) + gas(JAL)`. Setup and loop time remain in the
measurement while their gas is omitted from that budget. Jump rows do not
contribute to the normalizer. Layout coverage does not guarantee host branch
mispredictions or a global worst case.

## Gas units and validation

The normalizer is the median usable saturated single-opcode cell in the run,
with the filler baseline retained for older JSON. The default resolution is one
gas per baseline instruction, shared by both reporters. A different
`--resolution=N` requires both schedules and gas limits to use that unit.

For repeated runs, the instruction reporter takes the median for each context
on each host, then the maximum across hosts and contexts. It uses the smallest
baseline across runs for the gas unit. Thus mixing a uniformly slower host with
a faster one can inflate the whole proposal. Near-baseline costs within the
reporter's margin receive the baseline price; other prices round up.

`--predict` compares mixed workloads and pairs against the sum of measured
per-opcode costs. Its matched-context prediction diagnoses interactions;
its worst-context bound checks whether the measured cases are covered by the
pricing model. Store/load forwarding pairs distinguish same-address forwarding
from a disjoint-address control. `--workloads` reports measured time per gas and
the spread across the workload corpus, including fused code.

Validation workloads must build, complete, and consume the expected gas for
setup, iterations, and termination. At least three successful samples are
required. Historical JSON without execution verification remains readable, but
cannot pass `--check` or `--predict`, or generate a new reference. Missing
workloads or participating opcodes, partial samples, and invalid timings fail.
Bounds allow a 5% measurement margin. Relative guards cannot detect a uniform
slowdown of the interpreter and workloads; that requires an absolute budget on
a specified host.

## Syscall measurements

The ISA-only harness measures an ECALL crossing through a synthetic `Host`:
suspend, host dispatch, gas/register commit, and resume. It excludes
`syscall_entry`, argument parsing, guest-pointer checks, coroutine allocation,
and handler work. Its shapes add selector reloads and small output writes.
`seam` echoes the selector in a0 and omits the reload; `selector_reload` models
the ABI's status return overwriting a0. Call count, write success, run boundaries,
and gas are checked before reporting a timing.

`rv32_syscall_entry_bench.cc` measures the remaining production path:

| Rows | Timed work |
|---|---|
| `entry/` | Direct `syscall_entry` call, argument checks, coroutine, and handler |
| `vm/` | Guest argument setup, ECALL, entry path, and resume |
| `buffers/` | Hash-stream creation across table occupancies, without syscall entry |
| `frame/` | Frame setup/teardown, first touches, parent walks, and child commit/discard |
| `cold/` | Snapshot-cache misses through the local socket to the fixture's database server |

The difference between `vm/` and `entry/` includes selector reload, loop control,
and amortized initial argument setup. It cross-checks the synthetic seam; it
does not isolate a universal fixed crossing cost. Warm entry rows reuse state; creation, first-touch, and cold rows
exercise the paths that reuse would hide. Storage sweeps are read per key at
each occupancy, rather than assuming one linear fit across cache sizes.

Hashing is priced per compression block; stream creation is measured separately.
Bytestring reports examine adjacent length bands because copy cost per byte
changes with size. Signature verification includes valid signatures for each
message length. See [MEASUREMENTS.md](MEASUREMENTS.md) for retained evidence
behind the committed prices and the limits of those runs.

## Drift checks

```sh
scripts/rv32_gas_check.sh "$B"
scripts/rv32_gas_check.sh "$B" --fingerprint-only
scripts/rv32_syscall_report.py /tmp/sys.json --check \
  hogd/hog_exec/runtime/rv32/gas_calibration/syscall_reference.txt
scripts/rv32_gas_report.py /tmp/gas.json --emit-reference > \
  hogd/hog_exec/runtime/rv32/gas_calibration/reference.txt
```

The opcode check uses `reference.txt`; the production syscall check uses
`syscall_reference.txt` and the output from `rv32_syscall_entry_bench`. Syscall
drift ratios use the measured crossing cost as their normalizer. Gas-price
proposals instead use the opcode run's median saturated cost as the common unit.

The behavioural fingerprint hashes the gas schedule, structural limits, and
fusion decisions from a fixed corpus. It cannot detect all timing changes.
A source digest separately covers interpreter source changes, including comments;
it does not by itself prove a performance change. Review compiler, flags, host,
revision, fingerprint, and source digest before comparing runs.

The opcode `--check` fails on a fingerprint mismatch, guarded cost regressions, workload or
interaction bound violations, and missing required coverage. A source-digest
mismatch warns. The committed timing tolerance is 100% for relative guards;
this is a coarse regression alarm. Ratios are host-class specific. Regenerate
references only after reviewing the change and deciding whether prices need to
follow it.

The RV32 Gas Calibration workflow and schedule (`hogd-rv32-gas-calibration.yml`
and `hogd/rv32-gas-calibration.toml` in `thruputchain/benchmarks`) run the
checks on a selected bench host. `mode=reference` skips comparison with
committed references; workload execution, coverage, and bound checks still
apply. JSON is stored in `metrics_history` under `metrics/rv32-gas-calibration/`;
repeated runs of one commit replace that commit's sample.

## Scope and schedule changes

Memory kernels use the default guest memory size, not the runtime's configurable
ceiling. They do not establish prices for every permitted memory footprint.
`FENCE`, `EBREAK`, and `UNIMP` are outside opcode sweeps. Measured contexts and
workloads are samples of reachable behavior, not proof of the worst case.
The local database fixture does not measure a remote database hop. Frame rows
measure child merge/discard; they exclude the root transaction's database commit.

`--schedule` prints a proposal. [`program.h`](../program.h) and [`gas_cost_table.h`](../../gas_cost_table.h) own
the committed constants. A price change is a consensus change and has to
update the schedule `rv32_vm_test` pins, but it touches no artifact, lowerer
or fixture: artifacts do not declare prices, and every artifact executes at
the build's current ones.

The main implementation files are `kernel.*` (instruction shapes),
`lowering_audit.*` (lowered-body checks), `measure.*` (timing and fits),
`provenance.*` (run identity), `syscall_kernel.*` (synthetic ECALL shapes),
`workloads.*` (mixed programs), and `rv32_gas_calibrate.cc` (CLI and JSON).
