# RISC-V Contract Builds Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build every integration-test contract to an RV32 artifact (`<name>.rv32a` + `<name>.rv32.json`) as part of the normal build, mirroring the existing WASM build.

**Architecture:** Single-package dual-target — each existing contract crate gains a `[[bin]]` target (`src/main.rs`) so `cargo build --bin <name>` produces an RV32 ELF that `build_risc.sh` lowers, while `groundhog build` keeps producing the WASM cdylib from the same crate. A new `add_riscv_builds(...)` CMake function drives one `build_risc.sh` invocation per contract, hung off the existing `build_all_contracts ALL` target. The SDK auto-wires `groundhog-rv32-runtime` on the riscv target, so each `main.rs` is a 4-line wrapper.

**Tech Stack:** CMake, Cargo (`-Zbuild-std`, nightly), `groundhog` CLI (WASM), `groundhog-rv32-lower` (RV32), shell.

**Spec:** `docs/superpowers/specs/2026-05-31-riscv-contract-builds-design.md`

---

## File Structure

- `built_contracts/Cargo.lock` — re-pinned to the landed SDK rev (via `cargo update`).
- `built_contracts/integration_test_contracts/<name>/Cargo.toml` — gains a `[[bin]]` block (per contract).
- `built_contracts/integration_test_contracts/<name>/src/main.rs` — new 4-line rv32 bin wrapper (per contract).
- `built_contracts/CMakeLists.txt` — new `add_riscv_builds(...)` function + call + ALL wiring.
- `built_contracts/build_risc.sh` — unchanged.

The WASM contract list lives in `add_wasm_builds(...)` in `built_contracts/CMakeLists.txt:202-238`. The rv32 list mirrors it.

---

## Task 1: Bump SDK and fetch new crates

**Files:**
- Modify: `built_contracts/Cargo.lock`

- [ ] **Step 1: Update the SDK git deps**

Run (network required):
```bash
cd built_contracts && cargo update -p sdk -p groundhog-rv32-runtime
```
Expected: `Updating git repository ...` and `Updating sdk ... -> ...` lines; `Cargo.lock` changes.

- [ ] **Step 2: Fetch crates for offline builds**

The builds run `--frozen --offline`, so the new rev's crates must be in the cargo cache.
Run:
```bash
./scripts/fetch.sh
```
Expected: `cargo fetch` succeeds for host, wasm32, and riscv32im targets; `groundhog-cli@0.4.3` and `groundhog-rv32-lower@0.4.5` already installed (no error).

- [ ] **Step 3: Verify the lock moved**

Run: `cd built_contracts && git diff --stat Cargo.lock`
Expected: `Cargo.lock` shows changed `sdk`/`groundhog-rv32-runtime` source revs.

- [ ] **Step 4: Commit**

```bash
git add built_contracts/Cargo.lock
git commit -m "build(contracts): bump SDK to rev with rv32 runtime auto-wiring"
```

---

## Task 2: Spike — add rv32 bin to `fibonacci`, prove both builds work

This is the make-or-break task. It verifies (a) `groundhog build` still produces the WASM cdylib with a `[[bin]]` present, and (b) `cargo build --bin` for the bare-metal riscv target does **not** choke on the contract's `cdylib` crate-type.

**Files:**
- Modify: `built_contracts/integration_test_contracts/fibonacci/Cargo.toml`
- Create: `built_contracts/integration_test_contracts/fibonacci/src/main.rs`

- [ ] **Step 1: Add the bin target to `fibonacci/Cargo.toml`**

Append after the existing `[lib]` block:
```toml
[[bin]]
name = "fibonacci"
path = "src/main.rs"
```

- [ ] **Step 2: Create `fibonacci/src/main.rs`**

```rust
//! RV32 binary wrapper for the `fibonacci` contract.
//!
//! The runtime (`_start`, panic handler, allocator, linker script) is
//! auto-wired by the SDK on the riscv32 target. This wrapper exists so
//! `cargo build --bin fibonacci` produces an ELF executable; the
//! `extern crate` force-links the contract rlib so its `#[contract]`-emitted
//! `gh_*` method-entry symbols land in `.text.gh_*` for the lowerer.
#![no_std]
#![no_main]

extern crate fibonacci;
```

- [ ] **Step 3: Verify the WASM build still works (cdylib tolerates the bin)**

Run: `cmake --build build -j16 --target all_wasms_built`
Expected: PASS; `build/built_contracts/fibonacci.wasm` and `fibonacci_spec.json` exist and are rebuilt without error.

- [ ] **Step 4: Verify the RV32 build works (bare-metal bin does not pull a failing cdylib)**

Run:
```bash
/bin/sh built_contracts/build_risc.sh fibonacci /tmp/rv32-spike
```
Expected: PASS; `/tmp/rv32-spike/fibonacci.rv32a` and `/tmp/rv32-spike/fibonacci.rv32.json` exist.

**If Step 4 fails with a `cdylib`/linker error for the riscv target:** STOP. The single-package approach is blocked by the `cdylib` crate-type. Do not proceed to Task 4. Report the exact error and the options (target-conditional crate-type, or separate `<name>-rv32` wrapper crates) to the user.

- [ ] **Step 5: Commit**

```bash
git add built_contracts/integration_test_contracts/fibonacci/Cargo.toml \
        built_contracts/integration_test_contracts/fibonacci/src/main.rs
git commit -m "build(contracts): add rv32 bin target to fibonacci (spike)"
```

---

## Task 3: Add `add_riscv_builds()` CMake function, wired for `fibonacci` only

**Files:**
- Modify: `built_contracts/CMakeLists.txt`

- [ ] **Step 1: Add the `all_riscv_contracts` target near the WASM targets**

After `built_contracts/CMakeLists.txt:34` (`add_dependencies(all_wasm_contracts all_wasm_headers)`), add:
```cmake
add_custom_target(all_riscv_contracts)
```

- [ ] **Step 2: Add the `add_riscv_builds` function**

Insert after the `add_wasm_builds` function (after `built_contracts/CMakeLists.txt:151`):
```cmake
# Build all RV32 contracts via build_risc.sh
# Usage: add_riscv_builds(CONTRACTS c1 c2 ...)
# Each contract produces <name>.rv32a and <name>.rv32.json from its [[bin]] target.
function(add_riscv_builds)
  cmake_parse_arguments(ARG "" "" "CONTRACTS" ${ARGN})

  set(common_depends
    ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.lock
    ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml
    ${roundtrip_rs_stamp})

  set(all_outputs "")
  set(all_depends ${common_depends})
  set(build_commands "")

  foreach(target_name ${ARG_CONTRACTS})
    list(APPEND all_outputs ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.rv32a)
    list(APPEND all_outputs ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.rv32.json)

    file(GLOB target_rs_files
      ${CMAKE_CURRENT_SOURCE_DIR}/integration_test_contracts/${target_name}/src/*.rs)
    list(FILTER target_rs_files EXCLUDE REGEX "roundtrip\\.rs$")

    list(APPEND all_depends
      ${CMAKE_CURRENT_SOURCE_DIR}/integration_test_contracts/${target_name}/Cargo.toml
      ${target_rs_files})

    list(APPEND build_commands
      COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/build_risc.sh
        ${target_name}
        ${CMAKE_CURRENT_BINARY_DIR})
  endforeach()

  add_custom_command(
    OUTPUT ${all_outputs}
    ${build_commands}
    DEPENDS ${all_depends}
    COMMENT "Building all RV32 contracts via build_risc.sh"
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  )

  add_custom_target(all_riscvs_built DEPENDS ${all_outputs})
  add_dependencies(all_riscv_contracts all_riscvs_built)
endfunction()
```

- [ ] **Step 3: Call it with `fibonacci` only (temporary), after the `add_wasm_builds(...)` call**

After `built_contracts/CMakeLists.txt:238` (the closing `)` of `add_wasm_builds`), add:
```cmake
add_riscv_builds(
  CONTRACTS
    fibonacci
)
```

- [ ] **Step 4: Wire into the top-level ALL target**

Change `built_contracts/CMakeLists.txt:303` from:
```cmake
add_dependencies(build_all_contracts all_wasm_contracts)
```
to:
```cmake
add_dependencies(build_all_contracts all_wasm_contracts all_riscv_contracts)
```

- [ ] **Step 5: Build and verify the rv32 artifact lands in the build tree**

Run: `cmake --build build -j16 --target all_riscv_contracts`
Expected: PASS; `build/built_contracts/fibonacci.rv32a` and `fibonacci.rv32.json` exist.

- [ ] **Step 6: Commit**

```bash
git add built_contracts/CMakeLists.txt
git commit -m "build(contracts): add_riscv_builds CMake function (fibonacci only)"
```

---

## Task 4: Roll out to all `SPEC_CONTRACTS`

`SPEC_CONTRACTS` are the 28 `#[contract]`-macro contracts in `add_wasm_builds`:
`fibonacci` (done), `fibonacci_call`, `cxs_roundtrip`, `txs_roundtrip`, `dev_wallet`, `deploy_and_transfer`, `read_only_test`, `manual_revert_test`, `manual_revert_caller`, `simple_wallet`, `simple_wallet_authclaim`, `sha256_wrapper`, `initial_token_claim`, `itc_erc20`, `authclaim_test`, `htlc`, `payment_channel`, `return_fault_test`, `tuple_return`, `no_syscall_test`, `syscall_misc_tests`, `syscall_parse_hardening_test`, `nnint_output_fault_test`, `output_alias_test`, `calldata_wrapper`, `delegate_call_test`, `ed25519_verify_wrapper`, `view_gateway_test`.

**Files (per contract `<name>`, excluding `fibonacci`):**
- Modify: `built_contracts/integration_test_contracts/<name>/Cargo.toml`
- Create: `built_contracts/integration_test_contracts/<name>/src/main.rs`

- [ ] **Step 1: For each contract, add the bin target to its `Cargo.toml`**

Append after the existing `[lib]` block:
```toml
[[bin]]
name = "<name>"
path = "src/main.rs"
```

- [ ] **Step 2: For each contract, create `src/main.rs`**

Identical template, only the `extern crate` name changes:
```rust
//! RV32 binary wrapper for the `<name>` contract. See fibonacci/src/main.rs.
#![no_std]
#![no_main]

extern crate <name>;
```

- [ ] **Step 3: Add all SPEC_CONTRACTS to the `add_riscv_builds(...)` call**

Replace the temporary `CONTRACTS fibonacci` list in `built_contracts/CMakeLists.txt` with the full SPEC_CONTRACTS list above (all 28 names).

- [ ] **Step 4: Build all rv32 contracts**

Run: `cmake --build build -j16 --target all_riscv_contracts`
Expected: PASS; `build/built_contracts/<name>.rv32a` exists for all 28.

**If a specific contract fails with `--deny warnings`:** fix the warning in that contract's source (it compiles for rv32 now), re-run, and note the fix in the commit.

- [ ] **Step 5: Commit**

```bash
git add built_contracts/integration_test_contracts/*/Cargo.toml \
        built_contracts/integration_test_contracts/*/src/main.rs \
        built_contracts/CMakeLists.txt
git commit -m "build(contracts): build all spec contracts for rv32"
```

---

## Task 5: Handle `RAW_CONTRACTS`

`RAW_CONTRACTS`: `hash_bug`, `calldata_raw`, `sha256_raw`, `fibonacci_raw`, `nocontract_wallet`. These have no `#[contract]` macro, so no `gh_*` entry symbols.

- [ ] **Step 1: Add bin target + main.rs to each raw contract (same template as Task 4)**

For each: append the `[[bin]]` block to `Cargo.toml` and create `src/main.rs` with `extern crate <name>;`.

- [ ] **Step 2: Add raw contracts to the `add_riscv_builds(...)` CONTRACTS list and build**

Run: `cmake --build build -j16 --target all_riscv_contracts`
Expected: one of two outcomes — observe which:
- PASS for all raw contracts (lowerer accepts entry-less ELF → keep them in the list), or
- FAIL for raw contracts (lowerer rejects ELF with no `gh_*` symbols).

- [ ] **Step 3: If raw contracts fail, exclude them with a logged note**

Remove the failing raw contracts from the `add_riscv_builds(...)` call and add, just before the call:
```cmake
# RAW_CONTRACTS (hash_bug, calldata_raw, sha256_raw, fibonacci_raw,
# nocontract_wallet) have no #[contract] macro and emit no rv32 entry symbols,
# so they are intentionally excluded from the rv32 build.
message(STATUS "RV32 contracts: skipping raw contracts (no #[contract] entry symbols)")
```
Leave the raw contracts' `[[bin]]`/`main.rs` in place only if they build cleanly standalone; otherwise revert those raw-contract source edits too.

- [ ] **Step 4: Final full build**

Run: `cmake --build build -j16 --target build_all_contracts`
Expected: PASS; both `<name>.wasm` and `<name>.rv32a` present for all included contracts.

- [ ] **Step 5: Commit**

```bash
git add -A built_contracts
git commit -m "build(contracts): handle raw contracts for rv32 build"
```

---

## Task 6: Push and open PR

- [ ] **Step 1: Push the branch**

```bash
git push -u origin geoff/riscv-contract-builds
```

- [ ] **Step 2: Open the PR**

```bash
gh pr create --title "Build every contract for RV32 alongside WASM" \
  --body "Integrates build_risc.sh into the normal build: each contract now produces <name>.rv32a + <name>.rv32.json alongside <name>.wasm. See docs/superpowers/specs/2026-05-31-riscv-contract-builds-design.md."
```

- [ ] **Step 3: Watch CI and fix failures**

Use the check-ci skill to monitor and address failures.

---

## Self-Review Notes

- **Spec coverage:** per-contract source changes (Tasks 2,4,5), `add_riscv_builds` + ALL wiring (Task 3), SDK bump (Task 1), validation gates — dual-target spike (Task 2 Step 4), raw contracts (Task 5), `--deny warnings` (Task 4 Step 4). `build_risc.sh` unchanged (no task). All covered.
- **Newly surfaced risk (not in spec):** the `cdylib` crate-type may make the bare-metal riscv bin build fail. Task 2 Step 4 is the explicit gate; if it fails, the single-package approach is blocked and the user must be consulted before Task 4.
