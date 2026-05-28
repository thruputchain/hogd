# RV32 Experimental VM Docs

## File Layout

- [rv32-vm-experimental-design.md](rv32-vm-experimental-design.md)
  - Current architecture and milestone status.
  - Best starting point for implementation state.
- [rv32-cpp-ownership-review.md](rv32-cpp-ownership-review.md)
  - "Do we like owning this C++ forever?" review.
  - Tracks closed and remaining M5 readiness gates.
- [rv32-rust-vm-lessons.md](rv32-rust-vm-lessons.md)
  - Prior-art notes from CKB-VM and PolkaVM.
  - Explains why the artifact/runtime boundary looks the way it does.
- [rv32-vm-baseline.md](rv32-vm-baseline.md)
  - Initial Release benchmark baseline versus wasmi.

This directory is the working record for the experimental in-tree RV32IM VM.
RV32 is still test-only: no `ContractType::kRv32`, no deployed-contract path,
and no protocol-visible behavior.

## Implementation Layers

- VM core: `program.{h,cc}`, `machine.{h,cc}`, `host.h`, `encoding.h`.
  - Validates RV32IM code, decodes once, executes deterministic instructions,
    meters gas, and exposes `ecall`.
- Host bridge: `syscall_bridge.{h,cc}`.
  - Converts RV32 memory-frame `ecall`s into the shared `syscall_entry` path.
- Artifact and dispatch: `artifact.{h,cc}`, `dispatch.{h,cc}`.
  - Parses the strict v3 artifact format, validates SHA256 section commitments,
    and maps method IDs to validated PCs.
- Experimental runtime: `runtime_context.{h,cc}`.
  - Test-only `ContractType::kMockTest` runtime used for RV32/WASM parity.
- Fixture/lowerer: `fixtures/compiled_sha256_return/`.
  - Disposable Rust/lowerer spike used as a regression fixture, not the final SDK.
- Tests and benchmark:
  - `rv32_vm_test.cc`: VM, artifact, gas, and instruction behavior.
  - `rv32_syscall_bridge_test.cc`: bridge atomicity and marshalling.
  - `rv32_syscall_entry_bridge_test.cc`: real `syscall_entry` bridge tests.
  - `rv32_runtime_context_test.cc`: RV32/WASM parity and outcome mapping.
  - `rv32_vm_bench.cc`: interpreter baseline.

## Current Status

Milestones 1-4 are complete for the experimental scope. The VM runs hand-built
programs and a compiled Rust fixture, uses a versioned artifact with dispatch,
and has RV32/WASM parity tests for success and error outcomes.

The core is solid enough to keep owning. Before protocol exposure, the remaining
work is activation hardening: cross-platform determinism runs, a production SDK
lowerer, an RV32I/RV32M conformance corpus, fuzzing/error-triage polish, and
explicit protocol discipline around artifact hashing and gas version bumps.
