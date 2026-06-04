# RV32 Experimental VM Docs

## File Layout

- [rv32-vm-experimental-design.md](rv32-vm-experimental-design.md)
  - Current architecture and milestone status.
  - Best starting point for implementation state.
- [rv32-cpp-ownership-review.md](rv32-cpp-ownership-review.md)
  - "Do we like owning this C++ forever?" review.
  - Tracks closed and remaining M5 readiness gates.
- [rv32-author-toolchain.md](rv32-author-toolchain.md)
  - Author-side tooling design: standalone lowerer (wasm-opt model), the
    3-repo split, and the guest-side dispatch decision.
  - Answers the "Production SDK lowerer design" gate from the ownership review.
- [rv32-guest-dispatch-v4.md](rv32-guest-dispatch-v4.md)
  - Execution contract for the guest-side dispatch cut (author-toolchain
    Phase 3): the `_start` entry ABI, unknown-method semantics, the v4 artifact
    layout, and cross-repo sequencing.
  - Proposes closure of author-toolchain open questions #1 (method-id passing)
    and #2 (v4); becomes final once Decision 2 is signed off.
- [rv32-rust-vm-lessons.md](rv32-rust-vm-lessons.md)
  - Prior-art notes from CKB-VM and PolkaVM.
  - Explains why the artifact/runtime boundary looks the way it does.
- [rv32-vm-baseline.md](rv32-vm-baseline.md)
  - Initial Release benchmark baseline versus wasmi.
- [rv32-conformance-and-performance.md](rv32-conformance-and-performance.md)
  - In-tree conformance scope and Linux-review benchmark counters.
- [rv32-upstream-test-plan.md](rv32-upstream-test-plan.md)
  - Step-by-step plan for using upstream `riscv-tests` artifacts, rejection
    fixtures, and benchmark workloads.
- [riscv-tests-license.md](riscv-tests-license.md)
  - License notice for adapted `riscv-tests` conformance vectors.

This directory is the working record for the experimental in-tree RV32IM VM.
RV32 is still test-only: no `ContractType::kRv32`, no deployed-contract path,
and no protocol-visible behavior.

## Implementation Layers

- VM core: `program.{h,cc}`, `machine.{h,cc}`, `host.h`, `encoding.h`.
  - Validates RV32IM code, decodes once, executes deterministic instructions,
    meters gas, and exposes `ecall`.
- Host bridge: `syscall_bridge.{h,cc}`.
  - Converts RV32 register-native `ecall`s (syscall number in `a0`, args in
    `a1`-`a7`) into the shared `syscall_entry` path.
- Artifact and dispatch: `artifact.{h,cc}`, `dispatch.{h,cc}`.
  - Parses the strict v3 artifact format, validates SHA256 section commitments,
    and maps method IDs to validated PCs.
- Experimental runtime: `runtime_context.{h,cc}`.
  - Test-only `ContractType::kMockTest` runtime used for RV32/WASM parity.
- Fixture/lowerer: `fixtures/compiled_sha256_return/`.
  - Disposable Rust/lowerer spike used as a regression fixture, not the final SDK.
- Tests and benchmark:
  - `rv32_vm_test.cc`: VM, artifact, gas, and instruction behavior.
  - `rv32_conformance_test.cc`: declarative RV32IM conformance corpus.
  - `rv32_borrowed_conformance_test.cc`: adapted `riscv-tests` vectors.
  - `rv32_spec_model_crosscheck_test.cc`: fixed-seed random programs checked
    against an in-tree test-only reference model.
  - `rv32_unsupported_profile_test.cc`: upstream-inspired unsupported-profile
    rejection fixtures at program and artifact boundaries.
  - `rv32_upstream_artifact_test.cc`: synthetic upstream-shaped RV32UI/RV32UM
    artifacts run through the runtime boundary.
  - `rv32_syscall_bridge_test.cc`: bridge atomicity and marshalling.
  - `rv32_syscall_entry_bridge_test.cc`: real `syscall_entry` bridge tests.
  - `rv32_runtime_context_test.cc`: RV32/WASM parity and outcome mapping.
  - `rv32_vm_bench.cc`: interpreter baseline.

## Current Status

Milestones 1-4 are complete for the experimental scope. The VM runs hand-built
programs and a compiled Rust fixture, uses a versioned artifact with dispatch,
and has RV32/WASM parity tests for success and error outcomes.

The core is solid enough to keep owning. Before protocol exposure, the remaining
work is activation hardening: an external-oracle differential pass,
cross-platform determinism runs, a production SDK lowerer, expanded RV32I/RV32M
conformance coverage, fuzzing/error-triage polish, and explicit protocol
discipline around artifact hashing and gas version bumps.
