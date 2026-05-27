# RV32 Experimental VM Docs

This directory collects the design notes, review notes, and benchmark records
for the experimental in-tree RV32 VM.

Recommended reading order:

1. [Experimental RV32 VM Design](2026-05-27-rv32-vm-experimental-design.md)
   - Main project spec and milestone tracker.
2. [RV32 Rust VM Prior Art Lessons](2026-05-27-rv32-rust-vm-lessons.md)
   - Lessons from local Rust VM experiments and external RISC-V VM designs.
3. [M4 Slice 1: Compiled Rust Runs With Parity](2026-05-27-rv32-m4-slice1-design.md)
   - Toolchain-viability slice that led into artifact and dispatch work.
4. [RV32 C++ Ownership Review](2026-05-27-rv32-cpp-ownership-review.md)
   - Maintainability review for owning the VM core versus vendoring.
5. [RV32 VM Benchmark Baseline](2026-05-27-rv32-vm-baseline.md)
   - Initial Release benchmark comparison against wasmi.
6. [RV32 VM Hardening Pass](2026-05-27-rv32-vm-hardening.md)
   - Early ASAN/UBSAN confidence pass before host bridge work.

Current status:

- M1-M4 are complete for the experimental, test-only scope.
- RV32 remains isolated behind `ContractType::kMockTest`.
- The next decision point is M5 readiness: hardening the host/runtime/artifact
  edges before any protocol exposure.
