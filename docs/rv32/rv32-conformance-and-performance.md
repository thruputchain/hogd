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
- Trap ordering for bad PCs, out-of-bounds memory, and out-of-gas before
  mutation; unaligned data access succeeds.
- ECALL halt behavior through the narrow `Host` interface.

Add new cases when a bug fix depends on instruction semantics, trap class,
writeback ordering, gas ordering, or PC behavior. Prefer one focused case per
observable rule.

`rv32_borrowed_conformance_test.cc` adds operand/result vectors adapted from
upstream `riscv-tests` (`https://github.com/riscv-software-src/riscv-tests`),
specifically selected `isa/rv32ui/*.S` wrappers, their included
`isa/rv64ui/*.S` bodies, and selected `isa/rv32um/*.S` tests from local
checkout commit `1eb47d946c55f55cab8653c224c2993acc0276bd`. It avoids
importing ELF harnesses or assembler dependencies, but keeps the upstream
arithmetic, immediate, branch, load/store, upper-immediate, jump, and RV32M edge
values as a regression net. The license notice for these vectors is in
`riscv-tests-license.md`.

`rv32_spec_model_crosscheck_test.cc` adds a deterministic test-only reference
model. It generates fixed-seed RV32IM programs with random registers and memory,
runs both the model and `Machine`, and compares registers, memory, PC, gas,
halt/trap state, and halt code. This is an in-tree cross-check, not an
independent external oracle; Spike/sail-riscv or another external oracle remains
desired before protocol-visible RV32 work.

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

The benchmark corpus now includes parsed artifact workloads for three runtime
shapes:

- `bm_rv32_artifact_arithmetic_control_flow`: deterministic arithmetic and
  branch-heavy guest work with a final return.
- `bm_rv32_artifact_memory_transform`: byte load/store copy-transform loop with
  a final return.
- `bm_rv32_artifact_compiled_sha256_return`: lowered Rust artifact using the
  syscall bridge for SHA256 and return.

Use `scripts/compare_benchmark_json.py` to turn before/after benchmark JSON
into a PR-ready table:

```sh
scripts/compare_benchmark_json.py /tmp/rv32-before.json /tmp/rv32-after.json \
  --filter '^bm_(dispatch_arithmetic|load_store_loop|rv32_fibonacci_recursive_20|rv32_artifact_)'
```

## Runtime Boundary Gate

Before protocol-visible RV32 work, keep this gate green:

- Borrowed conformance and in-tree spec-model cross-checks pass.
- External-oracle differential testing with Spike, sail-riscv, or equivalent is
  passing.
- Artifact parser rejection tests cover malformed real artifact bytes.
- Runtime invocation tests exercise
  `artifact -> parse_artifact() -> RuntimeContractImage -> RuntimeContext`.
- No-effect, memory-heavy, and host-call-heavy RV32 artifacts return stable
  outputs across repeated invocations.
- Missing-method and invalid-dispatch cases fail deterministically.
- Benchmark JSON includes the three artifact workload classes above.
- Production selection remains mock-only unless explicitly promoted.

## Optimization Log

Every VM optimization should keep the same discipline: release baseline,
focused change, conformance, release after-run, benchmark comparison, and a
local reviewer pass. Single before/after runs are directional only because CPU
scaling may be enabled on review machines.

| Optimization | Status | Dispatch CPU | Load/store CPU | Fib20 CPU | Notes |
|---|---|---:|---:|---:|---|
| Specialize RV32 memory load/store widths | Kept | 5,473 ns -> 5,503 ns (-0.55%) | 6,263 ns -> 6,110 ns (+2.44%) | 3,634,579 ns -> 3,436,290 ns (+5.46%) | `bm_load_store_loop` measured the word path; byte/halfword paths are conformance-covered but still need width-specific perf coverage before making speed claims. |
| Cache decoded instruction fetch in `Machine` | Not kept | 5,400 ns -> 5,294 ns (+1.96%) | 6,021 ns -> 6,029 ns (-0.12%) | 3,446,402 ns -> 3,336,487 ns (+3.19%) | Reviewer found a stale-span risk; after fixing lifetime/immutability concerns, final release numbers were mixed/flat. |
| Inline branch condition dispatch | Not kept | 5,581 ns -> 5,382 ns (+3.56%) | 6,136 ns -> 6,129 ns (+0.11%) | 3,462,189 ns -> 3,533,395 ns (-2.06%) | Conformance passed, but reviewer flagged duplicated consensus-sensitive target handling and Fib20 regressed. |

## Future Performance Candidates

Pause core VM performance work until the benchmark corpus covers more than the
current microbenchmarks. The next useful performance step is workload diversity:
add larger straight-line code, host-call-heavy code, memory-copy/hash-like code,
and lowered real contract fixtures before making more interpreter changes.

| Candidate | Expected Win | Risk | Recommendation |
|---|---:|---:|---|
| Add diverse RV32 benchmark programs | Indirect/high confidence | Low | Done for initial artifact-backed arithmetic/control-flow, memory-transform, and compiled host-call-heavy workloads; broaden further with real contract fixtures before more VM tuning. |
| Specialize load/store bounds checks by width | Small-medium | Medium | Defer until memory-heavy benchmarks exist; trap ordering is easy to obscure. |
| Store PC as instruction index internally | Medium | Medium-high | Defer; touches byte-addressed `pc()`, bad-PC traps, jump targets, and diagnostics. |
| Split `run()` into a fused execution loop | Medium | Medium | Defer; needs tests proving `run()` and manual `step()`/`complete_ecall()` stay equivalent. |
| Avoid full memory snapshot on ECALL | High for host-heavy workloads | High | Design separately with rollback-log tests; this is isolation-sensitive. |
| Computed-goto/direct-threaded interpreter | Potentially high | High | Avoid for this PR; portability and readability costs are too high for the first production shape. |
| Branchless `write_reg()` / rd==0 tricks | Small | Low-medium | Skip unless profiling shows x0 writeback is material. |
