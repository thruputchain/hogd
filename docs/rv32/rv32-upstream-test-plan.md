# RV32 Upstream Test Plan

This is the working plan for leveraging upstream `riscv-tests` beyond the
already-adapted operand/result vectors. The goal is to increase confidence at
the runtime boundary while RV32 remains protocol-invisible.

## Current Baseline

- `rv32_borrowed_conformance_test.cc` already transcribes selected RV32UI and
  RV32UM operand/result expectations from upstream `riscv-tests`.
- `rv32_spec_model_crosscheck_test.cc` provides an in-tree randomized
  cross-check, but it is not an independent external oracle.
- `rv32_upstream_artifact_test.cc` adds a first-pass runtime-boundary smoke
  suite for selected upstream-shaped RV32UI/RV32UM cases. These are hand-encoded
  synthetic artifacts, not compiled upstream assembly artifacts.
- External-oracle differential testing and cross-platform determinism remain
  open gates before protocol-visible RV32 work.

## Phase 1: Upstream ISA Artifact Runner

Status: partially landed. `rv32_upstream_artifact_test` covers the runtime
artifact path with synthetic upstream-shaped RV32UI/RV32UM cases. It does not
yet consume compiled upstream `riscv-tests` assembly or ELF-derived artifacts.

Run a small allowlist of upstream-derived RV32UI/RV32UM tests through:

```text
RV32 test artifact -> parse_artifact() -> RuntimeContractImage -> RuntimeContext
```

First-pass synthetic artifact smoke:

- [x] `rv32ui/simple` synthetic upstream-shaped artifact case.
- [x] `rv32ui/add` synthetic upstream-shaped artifact case.
- [x] `rv32ui/addi` synthetic upstream-shaped artifact case.
- [x] `rv32ui/beq` synthetic upstream-shaped branch case.
- [x] `rv32ui/jal` synthetic upstream-shaped artifact case.
- [x] `rv32ui/jalr` synthetic upstream-shaped artifact case.
- [x] `rv32ui/lw` synthetic upstream-shaped load case.
- [x] `rv32ui/sw` synthetic upstream-shaped store case.
- [x] `rv32um/mul` synthetic upstream-shaped RV32M case.

Remaining compiled/lowered upstream artifact work:

- [ ] Decide whether to compile upstream assembly in CI or check in generated
      artifact bytes.
- [ ] Preserve upstream pass/fail intent without importing privileged harness
      behavior the VM does not support.
- [ ] Replace or augment the synthetic smoke cases with compiled/lowered
      artifacts for the same allowlist.

Acceptance criteria:

- [x] First-pass tests execute through
      `build_artifact() -> parse_artifact() -> RuntimeContractImage ->
      RuntimeContext`, not hand-built `Program`s.
- [x] Dispatch lookup, method PC validation, VM execution, and runtime outcome
      mapping are exercised for successful pass paths.
- [x] Synthetic pass/fail behavior maps to deterministic runtime
      success/failure.
- [x] No production `ContractType::kRv32` selector is introduced.
- [ ] Compiled/lowered upstream pass/fail behavior maps to deterministic
      runtime success/failure.

## Phase 2: Unsupported Profile Rejection

Use upstream suites outside the supported RV32IM profile as rejection fixtures.
These should validate that artifact/program creation fails cleanly for unsupported
instructions instead of adding execution behavior.

Candidate sources:

- [ ] `rv32ua`: atomics and LR/SC.
- [ ] `rv32uc`: compressed instructions.
- [ ] `rv32uf`, `rv32ud`, `rv32uzfh`: floating point.
- [ ] `rv32uzba`, `rv32uzbb`, `rv32uzbc`, `rv32uzbkb`, `rv32uzbkx`,
      `rv32uzbs`, `rv32uzicond`: bitmanip/conditional extensions.
- [ ] `rv32mi` selected SYSTEM/CSR/privileged encodings.

Acceptance criteria:

- [ ] Unsupported instructions are rejected at the validation boundary.
- [ ] Rejection tests assert the expected `ProgramError` class where stable.
- [ ] Privileged tests are used only as encoding inspiration unless the VM
      grows explicit privileged-mode semantics.

## Phase 3: Workload Benchmarks

After the artifact runner exists, adapt selected upstream benchmark programs for
workload diversity rather than tuning a single microbenchmark.

Candidate workloads:

- [ ] `benchmarks/memcpy`: memory-copy density.
- [ ] `benchmarks/median` or `benchmarks/qsort`: branch/control-flow density.
- [ ] `benchmarks/vvadd` or `benchmarks/multiply`: arithmetic loop density.
- [ ] `benchmarks/dhrystone`: broad mixed workload, only if harness adaptation
      stays small and auditable.

Acceptance criteria:

- [ ] Benchmarks produce JSON through `rv32_vm_bench`.
- [ ] Counters expose instruction mix, load/store density, branches, jumps, and
      `ecall`s where applicable.
- [ ] Results are treated as visibility until the corpus covers more real
      contract shapes.

## License Discipline

Continue referencing upstream directly:

- Repository: `https://github.com/riscv-software-src/riscv-tests`
- Current inspected checkout:
  `1eb47d946c55f55cab8653c224c2993acc0276bd`

If future work vendors assembly, generated artifacts, or binaries, update
`riscv-tests-license.md` to describe exactly what was imported and retain the
required upstream notice.
