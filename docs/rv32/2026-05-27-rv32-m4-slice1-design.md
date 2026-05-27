# RV32 Milestone 4 — Slice 1: Compiled Rust Runs With Parity

## Context

Milestones 1 through 3 of the experimental RV32 VM are complete (see
[Experimental RV32 VM Design](2026-05-27-rv32-vm-experimental-design.md)). The
runtime is proven sound, but
every test so far has fed it a **hand-assembled** instruction image. Milestone 4
is the SDK/toolchain question: can the output of a real compiler survive contact
with this VM?

This document scopes the **first** M4 implementor slice. It is deliberately the
thinnest end-to-end vertical that answers the toolchain unknowns, not the final
M4 form. The prior-art recommendations in
[RV32 Rust VM Prior Art Lessons](2026-05-27-rv32-rust-vm-lessons.md) (artifact
format, dispatch table, typed SDK
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
asserting parity on every host-visible effect this contract actually produces:

- invoke status,
- return buffer,
- SHA256 syscall trace and arguments,
- Return syscall body,
- syscall trace and transaction stats,

plus same-input determinism.

This slice deliberately does **not** claim storage-write or event-body parity.
The SHA256-then-Return contract writes no storage and emits no events, so those
comparisons would be vacuous (`0 == 0`) and prove nothing — the same hole the M3
review flagged. Asserting them here would manufacture a misleading green. Proving
the storage and event paths requires a second compiled contract that actually
exercises them, captured as followup F5; it is intentionally out of Slice 1 so
the toolchain-viability proof is not entangled with runtime-effect breadth.

Green parity means the toolchain, memory model, entry convention, and syscall
round-trip are all validated. Anything beyond that proof is out of scope for this
slice.

SHA256-then-Return is chosen deliberately over a Return-only or pure-arithmetic
contract because it is the only shape that forces a real **syscall round-trip**
(unknown #4) while reusing the M3 parity harness for the effects it does produce.

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
   lowering tool, and writes the committed fixture plus its metadata sidecar (see
   Reproducibility). Documented and checked in; **not** wired into the CMake
   build.
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

## Reproducibility

Because the Rust source, linker script, lowering tool, and committed image can
otherwise drift silently, the fixture must be reproducible and self-describing:

- Regeneration from a clean checkout must produce a **byte-identical** image. The
  build is pinned (fixed `rustc`/target, deterministic flags, no timestamps or
  paths leaking into the output) so the fixture is a pure function of its
  inputs.
- A committed metadata sidecar records, at minimum: the contract source hash, the
  linker script hash, the lowering tool hash, the ELF hash, the lowered image
  hash, the `rustc` version, the target triple, and the build flags.
- The regeneration script fails if a freshly built image does not match the
  committed image and metadata, so drift is caught at regeneration time rather
  than discovered later.

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

### Lowering Validation Rules

The `pc == vaddr` scheme and the dead-low-bytes shortcut are only safe if no
data-bearing content hides in executable space and no unexpected memory-resident
sections exist. The lowering tool must therefore **reject** rather than silently
accept anything outside an explicit allowlist. It must:

- accept only a known section allowlist: `.text` (code image) and the
  data-bearing `SHF_ALLOC` sections placed above it (`.rodata`, `.srodata`,
  `.data`, `.sdata`, `.bss`, `.sbss`);
- reject any other `SHF_ALLOC` section, so a stray literal pool, jump table,
  `.srodata`-style constant, or compiler-introduced data section in or around
  `.text` is a hard failure, not dead-but-wrong bytes;
- reject any section carrying `SHF_EXECINSTR` other than `.text`, and reject any
  data-bearing bytes inside the `.text` virtual-address range;
- reject the presence of dynamic relocations or any unresolved relocation
  records — the image must be fully linked and position-fixed;
- verify `.text` starts at vaddr 0 with `_start` first, and that every
  data-bearing section's vaddr lies above `.text` and within `kMaxMemoryBytes`.

The Rust build is configured to keep constants and jump tables out of `.text`
(no in-text constant pools), and the lowering tool's allowlist is the backstop
that fails loudly if a toolchain change violates that assumption.

## Entry and Syscall ABI

- **Entry**: a single fixed entry through the linker-placed `_start` stub at
  `pc = 0`. No method-selector dispatch table in this slice. `Machine`'s `pc = 0`
  initial-state invariant is left untouched. `_start` **must not return**: a guest
  invocation terminates only by issuing the Return (or Exit) syscall, which the
  bridge maps to `HostResult::Halt`. Running off the end of the code image is
  `Trap::BadPc`, so a missing terminating syscall fails loudly rather than
  silently succeeding. `_start` relies on the VM-provided `sp` (memory size
  rounded down to 16 bytes) and assumes a 16-byte-aligned stack; it does not set
  up its own stack.
- **Syscall**: the contract reuses the **existing M2/M3 frame ABI unchanged** —
  `a0` points at a guest-memory frame, `a7` carries the bridge version, the frame
  holds the syscall number and ten 64-bit argument words followed by a result
  footer. The Rust glue only writes that frame and executes `ecall`; the bridge,
  parser, alias guard, tracing, and gas machinery are unchanged. Because the
  compiler is otherwise free to use `a0`/`a7`, the syscall is emitted as inline
  assembly that makes the ABI explicit:
  - the frame is built in guest memory at a frame pointer that is
    `alignof(uint64_t)`-aligned — the bridge already traps on a misaligned `a0`,
    so misalignment is a hard failure, not undefined behavior;
  - `a0` is loaded with the frame pointer and `a7` with the bridge version
    immediately before the `ecall`, in the same asm block;
  - the asm block marks `a0`–`a7` (and any temporaries it uses) as clobbered so
    the compiler does not assume they survive the call;
  - the result footer (`result_code`, `panic`, `gas_consumed`) is read back from
    the committed frame in linear memory **after** the `ecall` returns, not from
    a register;
  - on `HostResult::Continue`, `a0` holds the return value per the bridge
    convention.

## Out of Scope (Guards)

This slice does **not**:

- define an on-disk or versioned artifact format,
- add a method-selector dispatch table,
- add typed Rust syscall/entry wrappers or SDK ergonomics,
- write any files into sdk_rs,
- change `Machine`, `Program`, or the syscall bridge.

## Followups

> **Status (as of the M4 close):** F1, F2, and F5 have since landed — the
> versioned artifact format, the method dispatch table, and storage/event
> parity, respectively. F0, F3, and F4 remain open. This section is preserved as
> the point-in-time slice plan; the current state lives in the design doc's
> Milestone 4 status.

- **F0** — Migrate the crude author-side (contract, linker script, syscall-frame
  writes) out of hogd fixtures into sdk_rs as the real contract-authoring
  surface, once viability is proven. Tracked as an M4 followup here; not folded
  into the main spec's pre-M5 deferred-hardening list, since it is SDK structure
  rather than runtime hardening.
- **F1** — Define the versioned Groundhog artifact format (lessons-doc R1); emit
  side in sdk_rs, consume/validate side in hogd. This is M4 slice 2.
- **F2** — Replace the single fixed entry with a method-selector dispatch table
  (R2).
- **F3** — Typed Rust syscall/entry wrappers and ergonomic macros in sdk_rs (R3).
- **F4** — ABI-version mismatch tests (R4) and prior-art test corpus import (R5).
- **F5** — A second compiled contract that actually writes storage and emits
  events, proving storage-write and event-body RV32/WASM parity that the
  SHA256-then-Return contract cannot. Closes the parity gap this slice
  deliberately leaves open.

## Review Gate

- The parity and determinism tests pass — the Milestone 3 gate, now fed by
  compiled Rust instead of a hand-assembled image — for exactly the host-visible
  effects the contract produces, with no vacuous (`0 == 0`) storage/event
  assertions.
- CI needs no Rust toolchain; the committed fixture is sufficient.
- A reviewer confirms `Machine`, `Program`, and the syscall bridge are untouched
  and that no protocol-visible behavior changed.
- The lowering tool enforces the section allowlist and relocation rules, rejecting
  unexpected `SHF_ALLOC`/executable sections and any data-bearing content in
  `.text`.
- The regeneration script reproduces the committed fixture byte-for-byte from a
  clean checkout, and the committed metadata sidecar matches.
