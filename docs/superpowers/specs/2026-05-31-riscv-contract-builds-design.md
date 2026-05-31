# RISC-V Contract Builds: Integrate `build_risc.sh` into the Normal Build

**Date:** 2026-05-31
**Status:** Approved (pending spec review)

## Goal

Every integration-test contract that is currently compiled to WASM in
`built_contracts/CMakeLists.txt` should *also* be compiled to an RV32 artifact
as part of the normal build — mirroring how `build_wasm.sh` is wired today. The
RV32 build is driven by the existing `built_contracts/build_risc.sh`, which
already produces `<name>.rv32a` + `<name>.rv32.json` for the lone
`rv32_smoke_counter` contract.

In short: for each contract, produce both `<name>.wasm` and `<name>.rv32a`.

## Background / Why this isn't just a flag

WASM and RV32 produce fundamentally different artifacts:

- **WASM:** `groundhog build` compiles the contract crate as a **cdylib**. The
  contract entrypoints are exported WASM symbols directly from the library — no
  separate binary is needed.
- **RV32:** `build_risc.sh` runs `cargo build … --bin <name>`, producing a
  standalone **ELF executable** that `rv32-lower` then lowers to the `.rv32a`
  artifact. An ELF needs a `_start`, a panic handler, an allocator, and a linker
  script — none of which live in the contract library.

That glue is supplied by `groundhog-rv32-runtime`, and the canonical SDK pattern
(see the SDK's own `example-contracts/counter-rv32`, and the doc comment in
`crates/rv32-runtime/src/lib.rs`) is a thin **per-contract bin target** that:

1. pulls in `groundhog-rv32-runtime` (`_start` / panic / allocator / linker
   script), and
2. force-links the contract rlib so its `#[contract]`-macro-emitted `gh_*`
   method-entry symbols land in `.text.gh_*` for the lowerer to discover.

The single existing example in this repo, `rv32_smoke_counter`, is a **single
package** carrying both a `[lib]` and a `[[bin]]` (`src/main.rs`). This design
follows that single-package shape for every contract.

### SDK auto-wiring (prerequisite)

The SDK has been updated (out of this repo, in `sdk_rs`) so that
`groundhog-rv32-runtime` is auto-wired on the `riscv32` target. Consequently the
per-contract `main.rs` needs **only one** `extern crate <contract>;` line — it no
longer needs an explicit `extern crate groundhog_rv32_runtime;` nor a
`groundhog-rv32-runtime` dependency line in `Cargo.toml`.

This design depends on `built_contracts/Cargo.lock` being re-pinned to that
updated SDK revision (see "SDK bump" below). Without it, the rv32 link step fails
to resolve `_start`.

## Naming convention

- **Package / lib name:** unchanged (e.g. `fibonacci`).
- **rv32 bin name:** equal to the package name (e.g. `fibonacci`). Because each
  contract package name is already unique across the workspace, the bin names are
  unique automatically — no `_rv32` suffix is needed. (The concern was a literal
  collision like every bin being named `riscv`; using the package name avoids
  that.)
- **Output artifacts:** `<contract>.rv32a` + `<contract>.rv32.json`, written into
  the build directory alongside `<contract>.wasm`.

This matches `rv32_smoke_counter` exactly (`-p $X --bin $X` → ELF at
`.../release/$X` → `$X.rv32a`), which is why **`build_risc.sh` needs no change**.

## Scope

All contracts currently listed in `add_wasm_builds(...)` get an rv32 build —
both `SPEC_CONTRACTS` and `RAW_CONTRACTS` ("every contract", per the request).

The `RAW_CONTRACTS` (`hash_bug`, `calldata_raw`, `sha256_raw`, `fibonacci_raw`,
`nocontract_wallet`) do not use the `#[contract]` macro, so they emit no `gh_*`
entry symbols. Their rv32 ELF will therefore have no contract entrypoints. See
the validation gates — if `rv32-lower` rejects an entry-symbol-less ELF, those
contracts are excluded from the rv32 build with a logged note (no silent drop).

## Design

### 1. Per-contract source changes (single-package dual-target)

For each contract crate, mirroring `rv32_smoke_counter`:

**`Cargo.toml`** — add a bin target:

```toml
[[bin]]
name = "<contract>"
path = "src/main.rs"
```

(The `entrypoints` feature already exists on every contract — it is what the
WASM build passes via `-F entrypoints` — so no feature changes are required.)

**`src/main.rs`** — new file, the trivial wrapper:

```rust
#![no_std]
#![no_main]

// Runtime (_start, panic handler, allocator, linker script) is auto-wired by
// the SDK on the riscv32 target. Force-link the contract lib so its
// #[contract]-macro-emitted gh_* entry symbols land in .text.gh_*.
extern crate <contract>;
```

No `groundhog-rv32-runtime` dependency line and no second `extern crate`.

### 2. `build_risc.sh`

**Unchanged.** It already accepts `<target_name> <target_dir>`, builds
`-p $X --bin $X`, finds the ELF at
`${INTERNAL_TARGET_DIR}/${TARGET_TRIPLE}/release/$X`, and emits `$X.rv32a` /
`$X.rv32.json` via `rv32_make_smoke_artifact.sh`.

### 3. CMake (`built_contracts/CMakeLists.txt`)

Add an `add_riscv_builds(CONTRACTS …)` function that mirrors `add_wasm_builds`:

- For each contract, declare outputs `${CMAKE_CURRENT_BINARY_DIR}/<name>.rv32a`
  and `<name>.rv32.json`.
- Dependencies: the same `common_depends` (`Cargo.lock`, `Cargo.toml`, the
  roundtrip stamp), plus each contract's `Cargo.toml`, its `src/*.rs` (now
  including the new `main.rs`).
- `build_commands`: one `build_risc.sh <name> ${CMAKE_CURRENT_BINARY_DIR}` per
  contract.
- A single `add_custom_command(OUTPUT <all rv32 outputs> …)` followed by an
  `all_riscv_contracts` custom target depending on those outputs.
- Set `ADDITIONAL_CLEAN_FILES` already covers `cargo-target`.

Wire it into the existing top-level ALL target:

```cmake
add_dependencies(build_all_contracts all_wasm_contracts all_riscv_contracts)
```

`add_riscv_builds(...)` is called with the same contract list as
`add_wasm_builds(...)` (modulo any contracts excluded per validation gate 2).

### 4. SDK bump

Re-pin `built_contracts/Cargo.lock` to the updated SDK revision that auto-wires
`groundhog-rv32-runtime` (the `sdk` / `groundhog-rv32-runtime` git deps resolve
to that rev). This is a prerequisite for the rv32 link step to succeed. Note:
the `EXPECTED_VERSION` gate in `build_wasm.sh` checks the groundhog *CLI*
version and is unrelated to this SDK pin.

## Validation gates (driven out during implementation)

These are real risks, validated rather than assumed away:

1. **Dual-target spike (do first).** Add the `[[bin]]` + `main.rs` to a single
   contract (`fibonacci`), then confirm:
   - `groundhog build` still produces `fibonacci.wasm` + `fibonacci_spec.json`
     (i.e. the added `[[bin]]` does not break the WASM build), and
   - `build_risc.sh fibonacci <dir>` produces `fibonacci.rv32a` +
     `fibonacci.rv32.json`.

   If `groundhog build` chokes on the bin target, stop and revisit the approach
   (fall back to separate `<name>-rv32` wrapper crates per the SDK example).

2. **Raw contracts.** Build the five `RAW_CONTRACTS` for rv32. If `rv32-lower`
   rejects an ELF with no `gh_*` entry symbols, exclude those contracts from
   `add_riscv_builds(...)` and emit a CMake `message(STATUS …)` noting the
   exclusion (no silent truncation).

3. **`--deny warnings`.** `.cargo/config.toml` sets `--deny warnings` for the
   `riscv32im-unknown-none-elf` target. Any contract that emits warnings when
   compiled for rv32 will fail; fix warnings as they surface.

## Testing / verification

- A clean `cmake --build` of the existing build directory produces a
  `<name>.rv32a` + `<name>.rv32.json` next to each `<name>.wasm`.
- `build_all_contracts` (ALL) triggers both `all_wasm_contracts` and
  `all_riscv_contracts`.
- The existing `rv32_smoke_counter` flow and `rv32_runtime_context_test` remain
  green (no regression to the established rv32 path).

## Out of scope

- Wiring the new `.rv32a` artifacts into C++ tests/consumers. The request is to
  *build* each contract for rv32; consuming the artifacts in tests is a
  follow-up.
- Any change to `build_risc.sh` or `rv32_make_smoke_artifact.sh`.
- SDK-side changes (already done in `sdk_rs`).
