# RV32 Conformance and Performance Checks

These checks treat the C++ RV32 VM as a production-candidate runtime while it
remains protocol-invisible.

## Conformance Corpus

The in-tree corpus lives in `rv32_conformance_test.cc` and is intentionally
small, readable, and dependency-free. It uses hand-encoded RV32IM programs so a
developer can audit the expected machine state without installing an assembler
or importing upstream test binaries.

Coverage includes:

- RV32I arithmetic, logical ops, shifts, upper immediates, branches, `jal`, and
  `jalr`.
- Little-endian loads/stores, sign extension, and memory writeback.
- RV32M multiply/divide/remainder edge cases, including divide-by-zero and
  signed overflow.
- Trap ordering for bad PCs, misaligned memory, and out-of-gas before mutation.
- ECALL halt behavior through the narrow `Host` interface.

Add new cases when a bug fix depends on instruction semantics, trap class,
writeback ordering, gas ordering, or PC behavior. Prefer one focused case per
observable rule.

`rv32_borrowed_conformance_test.cc` adds operand/result vectors adapted from
`riscv-tests` through PolkaVM commit
`bd2d14ed105467fd158b4744fd92f7045d288bb8`, path
`guest-programs/riscv-tests/tests`. It avoids importing ELF harnesses or
assembler dependencies, but keeps the upstream arithmetic, immediate, branch,
load/store, upper-immediate, jump, and RV32M edge values as a regression net.
The license notice for these vectors is in `riscv-tests-license.md`.

`rv32_differential_conformance_test.cc` adds a deterministic test-only
reference model. It generates fixed-seed RV32IM programs with random registers
and memory, runs both the model and `Machine`, and compares registers, memory,
PC, gas, halt/trap state, and halt code. It also includes explicit semantic trap
cases for bad control-flow targets, memory faults, and out-of-gas-before-mutate.
This is the fastest regression net to run before optimizing the VM core.

Run the conformance layers with:

```sh
ctest --test-dir build-release-ccache-tmp --output-on-failure -R '^rv32'
```

## Performance Counters

`rv32_vm_bench` reports wall time plus counters that make cross-machine runs
easier to compare:

- `rv32.guest_gas` and `rv32.host_gas`
- `rv32.loads`, `rv32.stores`
- `rv32.branches`, `rv32.branches_taken`, `rv32.jumps`
- `rv32.ecalls`, `rv32.traps`
- `rv32.bridge_memory_bytes` for syscall-bridge scratch-copy tests

Run:

```sh
build-release-ccache-tmp/hogd/hogexec/runtime/rv32/rv32_vm_bench \
  --benchmark_min_time=0.5s \
  --benchmark_out=/tmp/rv32-vm.json \
  --benchmark_out_format=json
```

Compare instruction throughput and the counter mix before treating a slowdown
as a core-dispatch regression. A syscall-bridge slowdown should show up in the
bridge round-trip benchmarks before it appears in compiled-contract parity
tests.
