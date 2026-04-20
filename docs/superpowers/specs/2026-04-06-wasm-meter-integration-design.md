# groundhog-wasm-meter Integration Design

## Context

The `groundhog-wasm-meter` crate (v0.1.1, published on the groundhog registry) provides deterministic wasm stack limiting via inline instrumentation. It adds a mutable i64 global `__groundhog_stack_limit` to a wasm module, exports it, and injects prologue/epilogue code in every function body that charges/refunds against this global (trapping via `unreachable` if the budget is exceeded).

The crate exposes two public functions:
- `instrument(wasm: &[u8]) -> Result<Vec<u8>, Error>` — rewrites a module to add stack limiting
- `verify(wasm: &[u8]) -> Result<(), Error>` — checks that a module is correctly instrumented

This design integrates the crate into hogd at three points:
1. **Production verify**: reject uninstrumented contracts at deployment time
2. **Runtime init**: set the stack limit budget when instantiating a module
3. **Test instrumentation**: instrument test contracts at load time so they pass verification

## Approach

Approach B from brainstorming: wasmi and wasmtime each depend on `groundhog-wasm-meter` directly for `verify()`. A new dedicated `wasm_meter` crate provides the `instrument()` FFI for C++ test code. Stack limit init is Rust-internal at instantiation time.

## 1. Cargo Dependency Wiring

### Registry config

Create `hogd/rust/.cargo/config.toml`:
```toml
[registries]
groundhog = { index = "sparse+https://groundhog.dev/crates/api/v1/crates/" }

[source]
groundhog = { registry = "sparse+https://groundhog.dev/crates/api/v1/crates/" }
```

### Workspace

In `hogd/rust/Cargo.toml`:
- Add to `[workspace.dependencies]`: `groundhog-wasm-meter = { version = "0.1.1", registry = "groundhog" }`
- Add `"wasm_meter"` to `members` list

### Crate dependencies

- `hogd/rust/wasmi/Cargo.toml`: add `groundhog-wasm-meter = { workspace = true }`
- `hogd/rust/wasmtime/Cargo.toml`: add `groundhog-wasm-meter = { workspace = true }`
- `hogd/rust/unified/Cargo.toml`: add `wasm_meter = { path = "../wasm_meter" }` dep
- `hogd/rust/unified/src/lib.rs`: add `extern crate wasm_meter;` to ensure the extern "C" symbols are linked into the staticlib

## 2. Production Verify Path

Rust-internal only, no new FFI.

### wasmi — `wasmi/src/wasmi_context.rs`

In `WasmiContext::verify_contract()`, after the existing `Module::new()` + `check_memory_limit()` checks pass:

```rust
if let Err(e) = groundhog_wasm_meter::verify(contract_data) {
    #[cfg(debug_assertions)]
    eprintln!("Contract verification failed: wasm-meter: {e}");
    return false;
}
```

### wasmtime — `wasmtime/src/context.rs`

In `WasmtimeContext::verify_contract()`, same pattern after module compile + memory limit check.

## 3. Stack Limit Global Init

Set the `__groundhog_stack_limit` exported global to **4096** (1/4 of the configured wasmtime `max_wasm_stack` of 16KB) at module instantiation time. This is Rust-internal with no FFI or C++ changes.

### wasmtime — `wasmtime/src/runtime.rs`

In `WasmtimeRuntime::new()`, after `instance_pre.instantiate(&mut store)` succeeds:

```rust
let stack_limit_global = instance
    .get_global(&mut store, "__groundhog_stack_limit")
    .expect("instrumented module must export __groundhog_stack_limit");
stack_limit_global
    .set(&mut store, wasmtime::Val::I64(4096))
    .expect("__groundhog_stack_limit must be a mutable i64 global");
```

### wasmi — `wasmi/src/wasmi_runtime.rs`

Same pattern using wasmi's global API after instance creation.

### Missing global

If the global is not found on an instance that passed `verify()`, this is a logic error — the `expect` / panic is appropriate.

## 4. Test Instrumentation

### New Rust crate: `hogd/rust/wasm_meter/`

**`Cargo.toml`**:
```toml
[package]
name = "wasm_meter"
version = "0.1.0"
edition = "2021"

[dependencies]
groundhog-wasm-meter = { workspace = true }

[lib]
crate-type = ["rlib"]
```

**`src/lib.rs`**: Two extern "C" functions:

- `wasm_meter_instrument(wasm_ptr: *const u8, wasm_len: usize) -> WasmMeterInstrumentResult`
  - Calls `groundhog_wasm_meter::instrument()`
  - On success: leaks the `Vec<u8>` into a raw pointer, returns `{ data, len, success: true }`
  - On failure: returns `{ null, 0, success: false }`
- `wasm_meter_free(ptr: *mut u8, len: usize)`
  - Reclaims allocation via `Vec::from_raw_parts`

**`WasmMeterInstrumentResult`** repr(C) struct:
```rust
#[repr(C)]
pub struct WasmMeterInstrumentResult {
    pub data: *mut u8,
    pub len: usize,
    pub success: bool,
}
```

**`cbindgen.toml`**: Same style as sibling crates (C++ language, `hogd` namespace, pragma_once). No `parse_deps` needed — this crate has no transitive types to expose.

**`CMakeLists.txt`**: cbindgen custom command + custom target, same pattern as `hogd/rust/crypto/CMakeLists.txt`.

### C++ RAII wrapper

**`hogd/hogexec/runtime/wasm/instrumented_bytes.h`**:

```cpp
namespace hogd {

// Owns bytes returned by wasm_meter_instrument(). Move-only.
class InstrumentedBytes {
public:
  // Returns nullopt on instrumentation failure.
  static std::optional<InstrumentedBytes> instrument(
      std::span<const uint8_t> wasm);

  std::span<const uint8_t> data() const;

  ~InstrumentedBytes();
  InstrumentedBytes(InstrumentedBytes &&other) noexcept;
  InstrumentedBytes &operator=(InstrumentedBytes &&other) noexcept;
  InstrumentedBytes(const InstrumentedBytes &) = delete;
  InstrumentedBytes &operator=(const InstrumentedBytes &) = delete;

private:
  InstrumentedBytes(uint8_t *data, size_t len);
  uint8_t *data_ = nullptr;
  size_t len_ = 0;
};

} // namespace hogd
```

**`hogd/hogexec/runtime/wasm/instrumented_bytes.cc`**: Implements the above, calling the `wasm_meter_instrument` / `wasm_meter_free` FFI functions from `hogd/rust/wasm_meter/bindings.h`.

### Test contract loaders

**`hogd/hogexec/runtime/wasm/test_load_wasm.h` — `load_raw_file()`**:
After reading file bytes into `contents`, instrument them:
```cpp
auto instrumented = InstrumentedBytes::instrument(
    std::span<const uint8_t>(
        reinterpret_cast<const uint8_t *>(contents.wasm.data()),
        contents.wasm.size()));
if (!instrumented) {
  throw std::runtime_error("failed to instrument wasm file: " + file);
}
contents.wasm.clear();
contents.wasm.append_range(instrumented->data());
```

**`built_contracts/contract_loader.cc` — `load_raw_file()`**:
Same pattern after reading file bytes.

### CMake wiring

- `hogd/hogexec/runtime/wasm/CMakeLists.txt`: Add `instrumented_bytes.cc` to the appropriate target, add include path for wasm_meter bindings.
- `built_contracts/CMakeLists.txt`: Add include path for wasm_meter bindings, link dependency.
- `hogd/rust/CMakeLists.txt`: Add the wasm_meter cbindgen target to the build.

## Files Changed

| File | Change |
|---|---|
| `hogd/rust/.cargo/config.toml` | New — groundhog sparse registry |
| `hogd/rust/Cargo.toml` | Add workspace dep, add wasm_meter member |
| `hogd/rust/wasmi/Cargo.toml` | Add groundhog-wasm-meter dep |
| `hogd/rust/wasmtime/Cargo.toml` | Add groundhog-wasm-meter dep |
| `hogd/rust/unified/Cargo.toml` | Add wasm_meter dep |
| `hogd/rust/unified/src/lib.rs` | Add `extern crate wasm_meter` |
| `hogd/rust/wasm_meter/` | New crate (Cargo.toml, src/lib.rs, cbindgen.toml, CMakeLists.txt) |
| `hogd/rust/wasmi/src/wasmi_context.rs` | Add verify() call |
| `hogd/rust/wasmtime/src/context.rs` | Add verify() call |
| `hogd/rust/wasmtime/src/runtime.rs` | Set __groundhog_stack_limit to 4096 after instantiation |
| `hogd/rust/wasmi/src/wasmi_runtime.rs` | Set __groundhog_stack_limit to 4096 after instantiation |
| `hogd/hogexec/runtime/wasm/instrumented_bytes.h` | New — RAII wrapper |
| `hogd/hogexec/runtime/wasm/instrumented_bytes.cc` | New — implementation |
| `hogd/hogexec/runtime/wasm/test_load_wasm.h` | Instrument after loading |
| `built_contracts/contract_loader.cc` | Instrument after loading |
| `hogd/hogexec/runtime/wasm/CMakeLists.txt` | Add instrumented_bytes, link wasm_meter |
| `built_contracts/CMakeLists.txt` | Link wasm_meter bindings |
| `hogd/rust/CMakeLists.txt` | Add wasm_meter cbindgen target |

## Not Changed

- `common` crate — no new dependency
- `Runtime` base class — no new virtuals
- `NativeRuntime` — unaffected
- C++ invoke path — stack limit is set in Rust at instantiation, not per-invoke
