# RV32 Rust VM Prior-Art Lessons

This note records what we borrowed from local CKB-VM and PolkaVM checkouts. It
is not a recommendation to vendor either VM.

## Main Lesson

The arithmetic interpreter is not the hard part. The hard parts are:

- artifact format;
- versioning;
- host boundary;
- gas accounting;
- deterministic failure classification;
- conformance and fuzzing.

That matches the Groundhog implementation: the C++ core stayed small, while the
artifact, dispatch, gas, and host-boundary edges took most review cycles.

## CKB-VM Lessons

- Version semantics from the first protocol artifact.
  - Groundhog now has artifact format, VM profile, entry ABI, syscall ABI, gas
    version, and gas digest fields.
- Charge before execute.
  - Groundhog charges instruction gas before mutation and host gas before host
    result commit.
- Treat PC update as a staged commit.
  - Groundhog validates jump targets before writing link registers or changing
    PC.
- Do not inherit a POSIX loader.
  - Groundhog runtime consumes a compact artifact; ELF is lowerer input only.
- Keep a bug graveyard.
  - Current tests are strong for local invariants; an RV32I/RV32M conformance
    corpus is still needed before protocol exposure.

## PolkaVM Lessons

- Raw compiler output should not be the runtime artifact.
  - Groundhog now uses a strict v4 artifact with code, initial memory, ABI, gas,
    and SHA256 commitments.
- Resolve entry and host linkage before execution.
  - Groundhog enters at PC 0 and passes the method ID to guest-side dispatch.
- Keep the VM unaware of high-level SDK names.
  - The VM receives a PC and an `ecall` frame; method names and selectors are
    runtime/artifact concerns.
- Typed host functions are SDK ergonomics, not VM core.
  - The current Rust fixture uses raw frames; production SDK should wrap them.
- One return path is easier to reason about.
  - Groundhog uses `Return`/`Exit` syscalls mapped to VM halt.

## Decisions That Landed

- Build a small in-tree C++ interpreter instead of vendoring a general VM.
- Keep ELF parsing out of runtime code.
- Use a Groundhog artifact as the execution input.
- Use guest-side dispatch from a fixed entry point, not WASM export lookup.
- Keep host calls as explicit register-native `ecall`s.
- Compare RV32/WASM at the host-visible result layer, not by raw gas equality.

## What Not To Copy

- Broad ISA support: atomics, compressed instructions, RV64, bitmanip, floats.
- POSIX-like argc/argv program loading.
- Dynamic imports or string-symbol host lookup in consensus runtime.
- JIT/AOT before interpreter determinism is fully established.
- Page-permission machinery unless a future profile needs it.

## Remaining Borrowed Work

- Import or recreate RV32I/RV32M conformance tests.
- Add parser/decoder/memory fuzzing.
- Run cross-platform determinism checks.
- Turn every future VM bug into a named regression.

## Source Checkouts Consulted

CKB-VM:

- `/Users/markmayo/code/gh/rv32/ckb-vm/README.md`
- `/Users/markmayo/code/gh/rv32/ckb-vm/src/machine/mod.rs`
- `/Users/markmayo/code/gh/rv32/ckb-vm/src/memory/mod.rs`
- `/Users/markmayo/code/gh/rv32/ckb-vm/src/cost_model.rs`
- `/Users/markmayo/code/gh/rv32/ckb-vm/tests/programs/`

PolkaVM:

- `/Users/markmayo/code/gh/rv32/polkavm/README.md`
- `/Users/markmayo/code/gh/rv32/polkavm/crates/polkavm/src/interpreter.rs`
- `/Users/markmayo/code/gh/rv32/polkavm/crates/polkavm/src/linker.rs`
- `/Users/markmayo/code/gh/rv32/polkavm/crates/polkavm/src/gas.rs`
- `/Users/markmayo/code/gh/rv32/polkavm/crates/polkavm-common/src/program.rs`
