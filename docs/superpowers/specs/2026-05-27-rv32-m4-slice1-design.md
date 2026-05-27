# RV32 Milestone 4 — Slice 1: Compiled Rust Runs With Parity

## Context

Milestones 1 through 3 of the experimental RV32 VM are complete (see
`2026-05-27-rv32-vm-experimental-design.md`). The runtime is proven sound, but
every test so far has fed it a **hand-assembled** instruction image. Milestone 4
is the SDK/toolchain question: can the output of a real compiler survive contact
with this VM?

This document scopes the **first** M4 implementor slice. It is deliberately the
thinnest end-to-end vertical that answers the toolchain unknowns, not the final
M4 form. The prior-art recommendations in
`2026-05-27-rv32-rust-vm-lessons.md` (artifact format, dispatch table, typed SDK
wrappers) describe the *eventual* design; this slice proves viability before we
take on that full-scope work, so we do not lock viability findings into a final
design prematurely.

WASM remains the production path. This slice adds no protocol-visible behavior.

## What This Slice De-Risks

Real compiled Rust raises four unknowns the hand-built M3 tests could not:

1. Does `riscv32im-unknown-none-elf` emit only instructions our decoder accepts
   (no atomics, no compressed, 4-byte aligned)?
2. Does real `no_std` Rust agree with our memory model — `.data`/`.bss`/stack
   placement, `sp` at top of memory, no assumed heap — inside 64 KiB?
3. Does a real compiler's prologue/calling convention reach our entry as we
   expect?
4. Can compiled Rust make a host syscall through our frame ABI and get the
   **same host-visible result as WASM**?

The thinnest vertical answers all four with minimum surface.

## Goal and Success Criterion

A `no_std` Rust contract reproducing Milestone 3's SHA256-then-Return behavior,
compiled to `riscv32im-unknown-none-elf` out of band, lowered host-side into the
existing `RuntimeContractImage`, run through the **unchanged** `rv32::RuntimeContext`,
asserting the **same parity Milestone 3 proved**:

- invoke status,
- return buffer,
- SHA256 syscall trace,
- Return syscall body,
- storage-write / event / transaction-stat counts and bodies,

plus same-input determinism.

Green parity means the toolchain, memory model, entry convention, and syscall
round-trip are all validated. Anything beyond that proof is out of scope for this
slice.

SHA256-then-Return is chosen deliberately over a Return-only or pure-arithmetic
contract because it is the only shape that forces a real **syscall round-trip**
(unknown #4) while reusing the M3 parity harness unchanged.

## The Consume/Author Boundary

A rule that recurs through M4/M5, recorded here so it is written down rather than
tribal:

- **hogd owns the *consume* side** — the VM, the `RuntimeContractImage` type,
  validation, the parity test, and (for this spike only) a throwaway ELF→image
  lowering helper. ELF parsing lives only in that helper; the runtime stays
  ELF-ignorant.
- **sdk_rs (`~/code/gh/sdk_rs`) will own the *author* side** — the ergonomic
  contract crate, entry/dispatch macros, typed syscall wrappers, and the blessed
  build target.

For this viability slice, the author side is reduced to its crudest possible
form — one hand-written `no_std` contract, a linker script, raw syscall-frame
writes — and kept **inside hogd as a clearly-labeled, disposable fixture**.
Building it properly in sdk_rs is the full-scope work this slice defers. Nothing
is written to sdk_rs in this slice; sdk_rs is the destination, and the migration
is captured as followup F0.

## Components

All components live in hogd. Only the C++ parity test is permanent; the rest is
disposable spike glue.

1. **Rust contract** — a minimal `no_std` crate: a `#[panic_handler]`, a
   `_start` entry, raw syscall-frame writes for the SHA256 and Return syscalls,
   and fixed buffers. No allocator, no heap.
2. **Linker script** — lays out the address space (see Memory and Address Model).
3. **Regeneration script** — invokes the Rust build out of band, runs the
   lowering tool, and writes the committed fixture. Documented and checked in;
   **not** wired into the CMake build.
4. **Lowering tool** — a host-side helper that parses the ELF, splits sections
   into code image versus initial linear memory by virtual address, and emits the
   fixture bytes. ELF parsing lives only here. It runs **only at regeneration
   time**, never in the CMake build or CI, so its implementation language is
   unconstrained and it is never linked into the parity test.
5. **Committed fixture** — the lowered code image plus initial-memory bytes,
   committed under the rv32 test tree.
6. **C++ parity test** — loads the fixture into `RuntimeContractImage` and reuses
   the Milestone 3 parity/determinism harness.

## Data Flow

```
Rust no_std contract  --(cargo, out of band)-->  riscv32im ELF
        |                                              |
        |                                       (lowering tool, host-side)
        |                                              |
        |                              code image  +  initial linear memory
        |                                              |
        +----------------------------------------> committed fixture (checked in)
                                                       |
                                          (C++ parity test, in CMake/CTest)
                                                       |
                                 RuntimeContractImage --> rv32::RuntimeContext
                                                       |
                                          parity + determinism assertions
```

CI runs only the right-hand side. It needs no Rust toolchain because the fixture
is committed; the regeneration script is run by hand when the contract changes.

## Memory and Address Model

This is the slice's primary technical risk and the area the implementor must get
exactly right. The VM is Harvard-style: `pc` indexes the separate immutable
`Program` instruction image, while loads and stores address linear `memory_`.
Both spaces are 0-based and physically distinct.

The consequence: PC-relative data access — `auipc` followed by a load, the
standard way RISC-V code reaches `.rodata`/`.data` — computes a value in
**pc-space** but the load consumes it as a **data-space** address. They agree
only if the link makes `pc == vaddr`.

Recommended layout:

- Place `.text` at **vaddr 0** with `_start` first, so `pc == vaddr` and
  `auipc`-based addressing resolves correctly into linear memory. The lowering
  tool extracts `.text` into the code image.
- Lay out `.rodata`, `.data`, `.bss`, and the stack at virtual addresses
  **above** `.text`, all within 64 KiB. The lowering tool places `.rodata` and
  `.data` bytes into the initial linear-memory image **at their virtual-address
  offsets** (not packed to zero). `.bss` is left zero because the VM zero-inits
  memory. The stack lives at the top; `Machine` already sets `sp` to the memory
  size rounded down to 16 bytes.
- Accepted consequence: the low `code_size` bytes of *linear* memory are dead,
  because code lives in the separate image. This is fine at this scale and is
  documented rather than worked around.

The parity test is the proof this layout is correct. A wrong virtual address or a
lowering mistake surfaces immediately as a SHA256 digest mismatch.

Both the code image and the linear-memory footprint must independently fit within
their existing limits (`kMaxCodeBytes` and `kMaxMemoryBytes`, both 64 KiB).

## Entry and Syscall ABI

- **Entry**: a single fixed entry through the linker-placed `_start` stub at
  `pc = 0`. No method-selector dispatch table in this slice. `Machine`'s `pc = 0`
  initial-state invariant is left untouched.
- **Syscall**: the contract reuses the **existing M2/M3 frame ABI unchanged** —
  `a0` points at a guest-memory frame, `a7` carries the bridge version, the frame
  holds the syscall number and ten 64-bit argument words followed by a result
  footer. The Rust glue only writes that frame and executes `ecall`; the bridge,
  parser, alias guard, tracing, and gas machinery are unchanged.

## Out of Scope (Guards)

This slice does **not**:

- define an on-disk or versioned artifact format,
- add a method-selector dispatch table,
- add typed Rust syscall/entry wrappers or SDK ergonomics,
- write any files into sdk_rs,
- change `Machine`, `Program`, or the syscall bridge.

## Followups

- **F0** — Migrate the crude author-side (contract, linker script, syscall-frame
  writes) out of hogd fixtures into sdk_rs as the real contract-authoring
  surface, once viability is proven. Cross-referenced from the main spec's
  deferred-hardening list.
- **F1** — Define the versioned Groundhog artifact format (lessons-doc R1); emit
  side in sdk_rs, consume/validate side in hogd. This is M4 slice 2.
- **F2** — Replace the single fixed entry with a method-selector dispatch table
  (R2).
- **F3** — Typed Rust syscall/entry wrappers and ergonomic macros in sdk_rs (R3).
- **F4** — ABI-version mismatch tests (R4) and prior-art test corpus import (R5).

## Review Gate

- The parity and determinism tests pass — the Milestone 3 gate, now fed by
  compiled Rust instead of a hand-assembled image.
- CI needs no Rust toolchain; the committed fixture is sufficient.
- A reviewer confirms `Machine`, `Program`, and the syscall bridge are untouched
  and that no protocol-visible behavior changed.
- The regeneration script is documented and reproduces the committed fixture.
