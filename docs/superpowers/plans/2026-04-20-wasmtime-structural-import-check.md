# Wasmtime Structural Import Validation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the `"incompatible import type"` substring classifier in `WasmtimeContext::get_instance_pre` with a structural validator backed by a shadow manifest, so the wasmtime wrong-import-signature rejection path no longer depends on unstable library error text (issue #1533).

**Architecture:** Add a `HashMap<(String, String), u8>` of `(module, field) → nargs` to `WasmtimeGlobalContext`, populated inside `link_function` alongside the per-engine `Linker::func_wrap` calls. Before `linker.instantiate_pre(&module)`, iterate `module.imports()` and reject deterministically on any name miss, non-`Func` kind, wrong arity, non-`i64` param, or non-`[i32]` result. Delete the `UnknownImportError` downcast and the `INCOMPATIBLE_IMPORT_MSG` substring branch; `instantiate_pre` errors collapse to `kSystemUnrecoverable`.

**Tech Stack:** Rust (wasmtime 42.0.0), C++ gtest, wat2wasm fixtures.

**Spec:** [`docs/superpowers/specs/2026-04-20-wasmtime-structural-import-check-design.md`](../specs/2026-04-20-wasmtime-structural-import-check-design.md)

---

## File Structure

Files touched, responsibility each:

- **`hogd/rust/wasmtime/src/global_context.rs`** — adds `imports: HashMap<(String,String), u8>` on `WasmtimeGlobalContext`, populated in `link_function`; exposes `imports()` accessor.
- **`hogd/rust/wasmtime/src/context.rs`** — new structural `validate_imports` helper; `get_instance_pre` calls it before `instantiate_pre` and collapses the post-call match; deletes `UnknownImportError` downcast and `INCOMPATIBLE_IMPORT_MSG` constant.
- **`hogd/hogexec/runtime/wasm/wat/test_unknown_import.wat`** — new fixture: imports `notmarmot::something` as a function.
- **`hogd/hogexec/runtime/wasm/wat/test_wrong_import_kind.wat`** — new fixture: imports `marmot::syscall` as a global (known name, wrong kind).
- **`hogd/hogexec/runtime/wasm/CMakeLists.txt`** — two new `wat2wasm_target` entries and two new `add_dependencies` entries.
- **`hogd/hogexec/runtime/wasm/instantiate_test.cc`** — two new parametrized tests: `UnknownImportRejectsDeterministically`, `WrongImportKindRejectsDeterministically`.

Everything stays in the existing wasmtime Rust crate and wasm runtime test target. No new crates, no new source files on the C++ side.

---

## Context an implementing engineer should know up front

### Host import shape invariant

Every host fn registered by `WasmtimeGlobalContext::apply_link_entry` is one of `link_function_Nargs` (N ∈ 0..=8), each defined by the `link_function_nargs!` macro at `hogd/rust/wasmtime/src/global_context.rs:17-41`. The wrapped closure has signature:

```
|caller: Caller<'_, *mut c_void>, arg1: u64, ..., argN: u64| -> Result<i32, wasmtime::Error>
```

`Linker::func_wrap` derives the wasm `FuncType` from the closure, yielding `[i64; N] -> [i32]` (the `Result<_, Error>` error channel becomes a trap and is not part of the wasm type). The shadow manifest therefore only records `nargs`; everything else is implied by this invariant.

### Build commands

- Rust-only check: `cargo check -p wasmtime --manifest-path hogd/rust/Cargo.toml`.
- C++ tests: `./scripts/build.sh -d ~/build/hogd-debug -t Debug -c gcc wasm_runtime_test` (use the `build` skill; builds must run sequentially and must not be backgrounded or timed out — see user memory).
- Running the wasmtime instantiation tests:
  `~/build/hogd-debug/hogd/hogexec/runtime/wasm/wasm_runtime_test --gtest_filter='WasmEngines/InstantiateTest.*Wasmtime'`.

### Preserving TDD ordering

The new test fixtures require the shadow manifest + validator to already be present before they can flip from passing (current behavior) to failing (only one of the new tests, `WrongImportKindRejectsDeterministically`, could hypothetically regress to unrecoverable if the kind-check is ever dropped). The current code happens to handle `UnknownImportError` via a typed downcast, so the unknown-name test passes even before the fix — it is a **guard** against removing the downcast, not a red-test for the fix itself. The primary red test is the mutation-resistance check in Task 6: delete `INCOMPATIBLE_IMPORT_MSG`'s branch and watch the existing `WrongImportTypeSignatureMismatch/Wasmtime` test still pass. Order tasks accordingly.

---

## Task 1: Shadow-manifest field and accessor on `WasmtimeGlobalContext`

**Files:**
- Modify: `hogd/rust/wasmtime/src/global_context.rs:1-3` (imports), `:43-64` (struct), `:189-206` (`new`), `:217-224` (accessors), `:226-231` (`link_function`).

- [ ] **Step 1: Add `HashMap` import and the `imports` field**

Replace the imports block at lines 1–3:

```rust
use core::ffi::c_void;
use std::collections::HashMap;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;
```

Add a new field on `WasmtimeGlobalContext` (after the existing `next_engine_index` field, before the closing brace of the struct at line 64):

```rust
    /// Shadow of host imports registered into every per-engine linker.
    /// Keyed by (module_name, field_name), value is the arity used to
    /// pick the `link_function_Nargs` wrapper. All host imports share
    /// the shape `[i64; nargs] -> [i32]`, so arity alone is sufficient
    /// to structurally validate a contract's declared import type.
    imports: HashMap<(String, String), u8>,
```

- [ ] **Step 2: Initialize `imports` empty in `new`**

In `WasmtimeGlobalContext::new` at lines 189–206, add `imports: HashMap::new(),` to the struct literal:

```rust
    pub fn new(
        engine_count: usize,
        instance_pre_cache: InstancePreCache,
    ) -> WasmtimeGlobalContext {
        let engines: Vec<Engine> =
            (0..engine_count).map(|_| Self::new_engine()).collect();
        let linkers: Vec<Linker<*mut c_void>> =
            engines.iter().map(Linker::new).collect();
        Self {
            engines,
            instance_pre_cache,
            engine_count,
            linkers,
            next_engine_index: AtomicUsize::new(0),
            imports: HashMap::new(),
        }
    }
```

- [ ] **Step 3: Expose `imports()` accessor**

Add after the existing `instance_pre_cache()` accessor at line 222–224:

```rust
    pub fn imports(&self) -> &HashMap<(String, String), u8> {
        &self.imports
    }
```

- [ ] **Step 4: Populate `imports` in `link_function`**

Replace the existing `link_function` (lines 226–231) with a version that also inserts into the manifest. The single-writer contract is already enforced by `Arc::get_mut` in `wasmtime_global_link_nargs` at line 393, so no locking is needed.

```rust
    pub fn link_function(&mut self, entry: LinkEntry) {
        // Apply to all existing linkers
        for linker in &mut self.linkers {
            Self::apply_link_entry(linker, &entry);
        }
        self.imports.insert(
            (entry.module().to_string(), entry.function().to_string()),
            entry.nargs(),
        );
    }
```

- [ ] **Step 5: Cargo check**

Run: `cargo check -p wasmtime --manifest-path hogd/rust/Cargo.toml`
Expected: clean build, no new warnings.

- [ ] **Step 6: Commit**

```bash
git add hogd/rust/wasmtime/src/global_context.rs
git commit -m "wasmtime: shadow host imports in a manifest keyed by (module, field)

Follow-up to #1533: first step toward replacing the string-based
classifier in get_instance_pre with a structural check."
```

---

## Task 2: Structural import validator

**Files:**
- Modify: `hogd/rust/wasmtime/src/context.rs:1-14` (imports), new helper above `impl WasmtimeContext`, `:83-162` (`get_instance_pre`).

- [ ] **Step 1: Import the wasm types used by the validator**

At `hogd/rust/wasmtime/src/context.rs:7`, extend the wasmtime `use` to include the types needed:

```rust
use wasmtime::{Engine, ExternType, InstancePre, Linker, Module, ValType};
```

- [ ] **Step 2: Add the `validate_imports` free function above `impl WasmtimeContext`**

Insert after `check_memory_limit` (line 32), before the `WasmtimeContext` struct at line 39:

```rust
/// Structurally validate every import declared by `module` against the
/// host manifest. Returns `true` iff every import is a function with
/// the registered arity, all i64 params, and a single i32 result.
///
/// This replaces wasmtime's stringly-typed incompatible-import-type
/// classification (see issue #1533): all rejections here are
/// deterministic, no formatted error text is inspected anywhere.
fn validate_imports(
    module: &Module,
    imports: &std::collections::HashMap<(String, String), u8>,
) -> bool {
    for import in module.imports() {
        let key =
            (import.module().to_string(), import.name().to_string());
        let expected_nargs = match imports.get(&key) {
            Some(n) => *n as usize,
            None => {
                debug_log!(eprintln!(
                    "Unknown host import: {}::{}",
                    import.module(),
                    import.name()
                ));
                return false;
            }
        };
        let func_ty = match import.ty() {
            ExternType::Func(f) => f,
            other => {
                debug_log!(eprintln!(
                    "Import {}::{} declared as non-function: {other:?}",
                    import.module(),
                    import.name()
                ));
                let _ = other;
                return false;
            }
        };
        let params: Vec<ValType> = func_ty.params().collect();
        let results: Vec<ValType> = func_ty.results().collect();
        if params.len() != expected_nargs {
            debug_log!(eprintln!(
                "Import {}::{} arity mismatch: declared {}, expected {}",
                import.module(),
                import.name(),
                params.len(),
                expected_nargs
            ));
            return false;
        }
        if !params.iter().all(|p| *p == ValType::I64) {
            debug_log!(eprintln!(
                "Import {}::{} param type mismatch: {params:?}",
                import.module(),
                import.name()
            ));
            return false;
        }
        if results.len() != 1 || results[0] != ValType::I32 {
            debug_log!(eprintln!(
                "Import {}::{} result type mismatch: {results:?}",
                import.module(),
                import.name()
            ));
            return false;
        }
    }
    true
}
```

(The `let _ = other;` after the `debug_log!` keeps the match arm's binding live when `debug_assertions` is off and the macro expands to nothing.)

- [ ] **Step 3: Call `validate_imports` before `instantiate_pre`**

In `get_instance_pre` at lines 83–162, insert the validator call immediately after the `check_memory_limit` block (current line 114) and before the existing comment block at line 116:

```rust
        if !check_memory_limit(&module) {
            debug_log!(eprintln!("Module instantiation failed: Memory export verification failed"));
            return Ok(None);
        }

        if !validate_imports(&module, self.global_context.imports()) {
            debug_log!(eprintln!(
                "Module instantiation failed: import validation"
            ));
            return Ok(None);
        }

        // The wasmtime API doesn't expose a way to check if a module has a start function.
        // ...existing comment...
```

- [ ] **Step 4: Cargo check**

Run: `cargo check -p wasmtime --manifest-path hogd/rust/Cargo.toml`
Expected: clean build.

- [ ] **Step 5: Build and run existing tests**

Use the `build` skill to build `wasm_runtime_test` (Debug, gcc). Then run:

```
~/build/hogd-debug/hogd/hogexec/runtime/wasm/wasm_runtime_test --gtest_filter='WasmEngines/InstantiateTest.*Wasmtime'
```

Expected: all existing tests still pass. In particular, `WrongImportTypeSignatureMismatch/Wasmtime` must still pass — the new validator now rejects the wrong-type case before `instantiate_pre`, so the rejection happens even though the substring classifier is still present and about to be removed in Task 3.

- [ ] **Step 6: Commit**

```bash
git add hogd/rust/wasmtime/src/context.rs
git commit -m "wasmtime: structurally validate imports before instantiate_pre

Checks host import names against the shadow manifest from the global
context, then verifies the declared ExternType is a function with the
expected [i64; nargs] -> [i32] shape. Substring classifier and
UnknownImportError downcast are left in place for now; removed in a
follow-up commit."
```

---

## Task 3: Delete the string classifier and `UnknownImportError` downcast

**Files:**
- Modify: `hogd/rust/wasmtime/src/context.rs:120-153` (collapse `instantiate_pre` match).

- [ ] **Step 1: Replace the `match` with a single `map_err`**

In `get_instance_pre`, the current match (lines 120–153) contains the `UnknownImportError` downcast, the `INCOMPATIBLE_IMPORT_MSG` constant, and the substring check. Replace the full `let instance_pre = Arc::new(match self.linker().instantiate_pre(&module) { ... });` block with:

```rust
        let instance_pre = Arc::new(
            self.linker()
                .instantiate_pre(&module)
                .inspect_err(|_e| {
                    debug_log!(eprintln!(
                        "Unrecoverable: instantiate_pre failed: {_e}"
                    ))
                })
                .map_err(|_e| SystemErrorCode::kSystemUnrecoverable)?,
        );
```

- [ ] **Step 2: Cargo check**

Run: `cargo check -p wasmtime --manifest-path hogd/rust/Cargo.toml`
Expected: clean build. If the build warns about unused `UnknownImportError` or similar, remove the now-unused items.

- [ ] **Step 3: Build and run the full wasmtime instantiation suite**

Use the `build` skill, then run:

```
~/build/hogd-debug/hogd/hogexec/runtime/wasm/wasm_runtime_test --gtest_filter='WasmEngines/InstantiateTest.*Wasmtime'
```

Expected: every test passes, **including** `WrongImportTypeSignatureMismatch/Wasmtime`. This is the mutation-resistance acceptance criterion: the substring branch is gone and the regression test still passes, because rejection now happens in `validate_imports`.

- [ ] **Step 4: Commit**

```bash
git add hogd/rust/wasmtime/src/context.rs
git commit -m "wasmtime: drop string-based incompatible-import classifier (#1533)

get_instance_pre no longer inspects anyhow error text to distinguish
unknown-import and type-mismatch cases. Structural validation runs
before instantiate_pre, so any error from instantiate_pre is now
system-unrecoverable without ambiguity.

Fixes #1533."
```

---

## Task 4: New test fixture — unknown import name

**Files:**
- Create: `hogd/hogexec/runtime/wasm/wat/test_unknown_import.wat`
- Modify: `hogd/hogexec/runtime/wasm/CMakeLists.txt` (new `wat2wasm_target` and `add_dependencies` entry).

- [ ] **Step 1: Write the `.wat` fixture**

Create `hogd/hogexec/runtime/wasm/wat/test_unknown_import.wat`:

```
(module
  (import "notmarmot" "something" (func $sys (param i64 i64 i64 i64 i64 i64 i64 i64) (result i32)))

  (func (export "invoke")
    i64.const 0
    i64.const 0
    i64.const 0
    i64.const 0
    i64.const 0
    i64.const 0
    i64.const 0
    i64.const 0
    call $sys
    drop
  )

  (memory 1 1)
  (export "memory" (memory 0))
)
```

The signature matches the 8-arg host shape so wasmtime's typecheck, if ever reached, would classify this as an *unknown-import* case rather than a signature mismatch.

- [ ] **Step 2: Register the fixture in CMake**

In `hogd/hogexec/runtime/wasm/CMakeLists.txt`, add a new `wat2wasm_target` block after the existing `test_wrong_import_type_wasm` block at line 151–155:

```cmake
wat2wasm_target(
  test_unknown_import_wasm
  wat/test_unknown_import.wat
  ${CMAKE_CURRENT_BINARY_DIR}/test_unknown_import.wasm
)
```

Then add `test_unknown_import_wasm` to the `add_dependencies(wasm_runtime_test ...)` list (currently ending at line 181 with `all_wasm_contracts`):

```cmake
add_dependencies(
  wasm_runtime_test
  test_invoke_wasm
  test_error_handling_wasm
  test_invoke_arity_wasm
  test_set_memory_wasm
  test_instantiate_fail_wasm
  test_instantiate_unexported_memory_wasm
  test_no_start_wasm
  test_oob_data_segment_wasm
  test_oob_element_segment_wasm
  test_wrong_import_type_wasm
  test_unknown_import_wasm
  test_param_entrypoint_wasm
  all_wasm_contracts
)
```

- [ ] **Step 3: Build to verify wat2wasm succeeds**

Use the `build` skill to build `wasm_runtime_test`. Expected: success; the new `test_unknown_import.wasm` appears in the build directory.

- [ ] **Step 4: Commit**

```bash
git add hogd/hogexec/runtime/wasm/wat/test_unknown_import.wat hogd/hogexec/runtime/wasm/CMakeLists.txt
git commit -m "test: add test_unknown_import.wasm fixture for #1533 guard test"
```

---

## Task 5: New test fixture — known name, wrong kind

**Files:**
- Create: `hogd/hogexec/runtime/wasm/wat/test_wrong_import_kind.wat`
- Modify: `hogd/hogexec/runtime/wasm/CMakeLists.txt`.

- [ ] **Step 1: Write the `.wat` fixture**

Create `hogd/hogexec/runtime/wasm/wat/test_wrong_import_kind.wat`:

```
(module
  (import "marmot" "syscall" (global i64))

  (func (export "invoke"))

  (memory 1 1)
  (export "memory" (memory 0))
)
```

The module imports the registered host name `marmot::syscall` as a **global** rather than a function — the kind-check path.

- [ ] **Step 2: Register in CMake**

Add after the `test_unknown_import_wasm` block from Task 4:

```cmake
wat2wasm_target(
  test_wrong_import_kind_wasm
  wat/test_wrong_import_kind.wat
  ${CMAKE_CURRENT_BINARY_DIR}/test_wrong_import_kind.wasm
)
```

Then append `test_wrong_import_kind_wasm` to the `add_dependencies(wasm_runtime_test ...)` list beside `test_unknown_import_wasm`.

- [ ] **Step 3: Build to verify wat2wasm succeeds**

Use the `build` skill. Expected: success; the new wasm appears in the build dir.

- [ ] **Step 4: Commit**

```bash
git add hogd/hogexec/runtime/wasm/wat/test_wrong_import_kind.wat hogd/hogexec/runtime/wasm/CMakeLists.txt
git commit -m "test: add test_wrong_import_kind.wasm fixture for #1533 guard test"
```

---

## Task 6: New C++ tests

**Files:**
- Modify: `hogd/hogexec/runtime/wasm/instantiate_test.cc` (append new tests after `WrongImportTypeSignatureMismatch`, before the next unrelated test).

- [ ] **Step 1: Add `UnknownImportRejectsDeterministically`**

Insert after the existing `WrongImportTypeSignatureMismatch` test body (currently ending at line 175):

```cpp
// Regression guard for #1533: a contract that imports a name the host
// does not register must be rejected as a deterministic invalid contract
// (null runtime), not escalated to kSystemUnrecoverable. This guards
// against accidentally removing unknown-import handling when the
// string-based classifier was replaced with structural validation.
TEST_P(InstantiateTest, UnknownImportRejectsDeterministically)
{
  ASSERT_TRUE(link_fn("marmot", "syscall", &dummy_syscall_8args));

  Contract contract(ContractType::kWasmi);
  contract.wasmi_contract() =
      test::load_wasm_from_file("test_unknown_import.wasm");

  auto deployed_contract_hash = DeployedContractHash{
      .hash = sha256(contract),
      .contract_size =
          static_cast<uint32_t>(contract.wasmi_contract().wasm.size()),
      .contract_type = ContractType::kWasmi,
  };

  make_syscall_handler(contract);
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash,
                                                 get_syscall_handler());
  ASSERT_TRUE(runtime.has_value())
      << "unknown import caused a SystemError";
  EXPECT_FALSE(runtime.value());
}
```

- [ ] **Step 2: Add `WrongImportKindRejectsDeterministically`**

Insert directly after the previous test:

```cpp
// Regression guard for #1533: a contract that imports a registered host
// name but as the wrong kind of entity (e.g. a global instead of a
// function) must be rejected as a deterministic invalid contract.
TEST_P(InstantiateTest, WrongImportKindRejectsDeterministically)
{
  ASSERT_TRUE(link_fn("marmot", "syscall", &dummy_syscall_8args));

  Contract contract(ContractType::kWasmi);
  contract.wasmi_contract() =
      test::load_wasm_from_file("test_wrong_import_kind.wasm");

  auto deployed_contract_hash = DeployedContractHash{
      .hash = sha256(contract),
      .contract_size =
          static_cast<uint32_t>(contract.wasmi_contract().wasm.size()),
      .contract_type = ContractType::kWasmi,
  };

  make_syscall_handler(contract);
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash,
                                                 get_syscall_handler());
  ASSERT_TRUE(runtime.has_value())
      << "wrong-kind import caused a SystemError";
  EXPECT_FALSE(runtime.value());
}
```

- [ ] **Step 3: Build and run**

Build via the `build` skill, then:

```
~/build/hogd-debug/hogd/hogexec/runtime/wasm/wasm_runtime_test --gtest_filter='WasmEngines/InstantiateTest.*Wasmtime'
```

Expected: full suite passes, including the two new `*Wasmtime` variants. The `Wasmi` variants should also pass — wasmi's own import handling already distinguishes unknown/kind/signature cases via typed errors in `wasmi_runtime.rs:107-112` (see the comment in the existing `WrongImportTypeSignatureMismatch` test at line 148).

- [ ] **Step 4: Run the full test target**

Run `wasm_runtime_test` with no filter to confirm no regressions:

```
~/build/hogd-debug/hogd/hogexec/runtime/wasm/wasm_runtime_test
```

Expected: all tests pass.

- [ ] **Step 5: Commit**

```bash
git add hogd/hogexec/runtime/wasm/instantiate_test.cc
git commit -m "test: guard unknown-name and wrong-kind import rejections (#1533)

Adds two parametrized InstantiateTest cases that exercise the structural
import validator directly: one for a module that imports a name we do
not register, one for a module that imports a registered name as the
wrong entity kind."
```

---

## Self-review checklist (run after Task 6)

- [ ] Spec coverage: every "Design" subsection has at least one task that implements it (shadow manifest: T1; structural validator: T2; post-instantiate_pre collapse: T3; fixture assets: T4–T5; tests 1–5 from spec: all covered by T6 + existing `WrongImportTypeSignatureMismatch`).
- [ ] No remaining references to `INCOMPATIBLE_IMPORT_MSG`, `UnknownImportError` downcast, or the `WASMTIME-VERSION-SENSITIVE` comment block in `context.rs`.
- [ ] `validate_imports` accepts a correctly-typed `marmot::syscall` import (verified via the happy-path tests in existing suite).
- [ ] CMake `add_dependencies` list includes both new fixtures.
- [ ] No changes to `verify_contract`, C++ `WasmtimeContext`, wasmi runtime, or the FFI surface.
- [ ] Git log contains a clean per-task commit sequence (6 commits).

## Done criteria

1. `WasmEngines/InstantiateTest.WrongImportTypeSignatureMismatch/Wasmtime` passes without `INCOMPATIBLE_IMPORT_MSG` existing anywhere in the tree.
2. `UnknownImportRejectsDeterministically/Wasmtime` passes. (See deviations below on the second gtest.)
3. Full `wasm_runtime_test` target passes.
4. `grep -rn 'incompatible import type\|INCOMPATIBLE_IMPORT_MSG\|UnknownImportError' hogd/rust/wasmtime/` returns no hits.

## Deviations during execution

- **Task 2 code change:** the plan's template compared `ValType` values with `==`. Wasmtime 42 does not implement `PartialEq` on `ValType` (reference types have subtyping), so the implementation uses `ValType::is_i64()` / `ValType::is_i32()` predicate methods. Both are defined as `matches!(self, ValType::I64/I32)` in `wasmtime-42.0.0/src/runtime/types.rs`, so semantics are identical for numeric types.
- **Task 5 + Task 6 (superseded by commit `75a68485f`):** the plan called for a `test_wrong_import_kind.wat` fixture and a matching `WrongImportKindRejectsDeterministically` gtest. Task 6 discovered this is unreachable via the test helper: `test::load_wasm_from_file` calls `InstrumentedBytes::instrument`, which invokes wasm-meter, which unconditionally rejects non-function imports (`groundhog-wasm-meter-0.3.0/src/analysis.rs`). An existing comment at `instantiate_test.cc:236-240` already documents this invariant. Only `UnknownImportRejectsDeterministically` was added. A follow-up cleanup commit removed the orphan Task 5 fixture and bumped the adjacent wasm-meter version tag from `0.2.0` → `0.3.0` to match `hogd/rust/Cargo.toml`.
- The kind-check branch in `validate_imports` is retained as defense-in-depth — if wasm-meter ever relaxes the "function imports only" rule or is bypassed, wasmtime's runtime still refuses.
