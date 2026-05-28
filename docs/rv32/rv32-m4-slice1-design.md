# RV32 M4 Slice 1: Compiled Rust Viability

This is the point-in-time plan for the first M4 slice. It is kept because it
explains the memory/lowering model used by the compiled Rust fixture.

Current status: this slice and the later M4 followups are complete for the
experimental scope. See [rv32-vm-experimental-design.md](rv32-vm-experimental-design.md)
for current architecture.

## Question

Can real `no_std` Rust compiled to `riscv32im-unknown-none-elf` run through the
unchanged RV32 VM and match WASM on host-visible behavior?

## Slice Goal

Build one Rust contract that:

- compiles to RV32IM ELF;
- lowers into a committed RV32 fixture;
- performs SHA256 through the RV32 syscall frame ABI;
- returns through the shared host path;
- matches the WASM SHA256 wrapper on status, return buffer, trace body, and
  deterministic reruns.

This deliberately did not prove storage/event parity. Those paths were later
covered by runtime parity tests using contracts that actually emit effects.

## Consume/Author Boundary

- `hogd` owns the consume side:
  - VM;
  - artifact parser;
  - runtime parity tests;
  - fixture lowerer for this experiment.
- `sdk_rs` should own the future author side:
  - contract crate;
  - entry/dispatch macros;
  - typed syscall wrappers;
  - production lowerer.

The fixture under `hogd/hogexec/runtime/rv32/fixtures/compiled_sha256_return/`
is intentionally disposable SDK glue plus a permanent regression artifact.

## Memory Model

The VM is Harvard-style:

- PC indexes immutable `Program` code.
- Loads/stores address guest data memory.
- Code bytes are not executable guest memory.

The fixture layout therefore makes link-time virtual addresses match PC-space:

- `.text` starts at virtual address `0`.
- `.rodata`, `.data`, `.bss`, frames, buffers, and stack live above `.text`
  inside the 64 KiB memory limit.
- Low guest-memory bytes corresponding to `.text` are dead space.
- Initial memory is a full 64 KiB image, not sparse sections.

The artifact parser now enforces the full-memory-image rule.

## Lowering Rules

The fixture lowerer must fail loudly on unsupported compiler output:

- `.text` starts at `0`;
- no unresolved relocations;
- no unsupported allocated sections;
- no executable section other than `.text`;
- no overlapping data sections;
- no data/object symbols in `.text`;
- dispatch entry symbols resolve to valid `.text` PCs;
- build metadata records the relevant source/config hashes.

This is still a fixture lowerer, not the production SDK lowerer.

## ABI

The Rust fixture uses the same RV32 bridge ABI as the VM tests:

- guest writes a syscall frame in memory;
- `a0` points at the frame;
- `a7` carries the bridge version;
- inline assembly executes `ecall`;
- the guest reads the footer after the host returns.

Normal completion is the Return syscall. `_start` must not return.

## What Landed After This Slice

- Versioned v3 Groundhog artifact.
- SHA256 section commitments.
- Runtime-side dispatch table from method ID to PC.
- Compiled multi-method fixture with distinct PCs.
- Unknown-method zero-gas deterministic miss.
- Storage/event parity tests.
- Differential error outcome tests.

## Still Not The Final SDK

Remaining SDK work:

- method manifest or generated metadata section;
- typed syscall wrappers;
- typed entry/dispatch macros;
- reproducible production lowerer;
- explicit `.text` data policy for future codegen.
