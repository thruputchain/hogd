# RV32 Experimental VM Docs

An experimental, in-tree RV32IM interpreter for Groundhog, built as a hedge
against depending solely on the WASM/wasmi execution path. These notes capture
the design, the prior-art reasoning, and the viability evidence.

Read this first — the shape of the experiment:

- The **VM core is deliberately small, explicit, and auditable.** A C++ ownership
  review concluded it is solid enough to own as consensus-adjacent code (see the
  ownership review below).
- From that core we **worked outward through milestones — toolchain, artifact
  format, method dispatch — specifically to determine viability**, not to ship a
  finished product.
- The **edges are intentionally not the final shape.** The host/runtime/artifact
  boundaries carry known pre-protocol cleanup, tracked as M5 gates. RV32 stays
  isolated behind `ContractType::kMockTest` and is not protocol-visible.

Recommended reading order:

1. [Experimental RV32 VM Design](2026-05-27-rv32-vm-experimental-design.md)
   - Main project spec, design decisions, and milestone tracker.
2. [RV32 Rust VM Prior Art Lessons](2026-05-27-rv32-rust-vm-lessons.md)
   - Why build in-tree rather than vendor, and what to borrow from CKB-VM and
     PolkaVM.
3. [M4 Slice 1: Compiled Rust Runs With Parity](2026-05-27-rv32-m4-slice1-design.md)
   - The toolchain-viability slice and the (non-obvious) memory/address model it
     established, which led into the artifact and dispatch work.
4. [RV32 C++ Ownership Review](2026-05-27-rv32-cpp-ownership-review.md)
   - The "would we want to own this forever?" read: the core is solid, the edges
     are M5 gates.
5. [RV32 VM Benchmark Baseline](2026-05-27-rv32-vm-baseline.md)
   - Initial Release-build wall-time comparison against wasmi (viability
     evidence).

Current status: Milestones 1-4 are complete for the experimental, test-only
scope. The next decision point is M5 readiness — hardening the
host/runtime/artifact edges before any protocol exposure. The full gate list is
the M5 Readiness Checklist in the ownership review.
