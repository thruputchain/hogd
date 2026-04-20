# Migrate Contract Build System to groundhog-cli

## Problem

The current build pipeline uses two separate cargo invocations:
1. `build_wasm.sh` — cross-compiles contracts to wasm32 via raw `cargo build`
2. `build_wasm_spec.sh` — builds native `generate_spec_*` / `generate_types_*` binaries, then runs them to produce spec JSON and `.x` type files

This requires every contract to ship `src/bin/generate_spec.rs` (and optionally `generate_types.rs`) binaries. The spec format has also changed with SDK updates, breaking `generate_header.py`.

`groundhog build` (from `groundhog-cli`) replaces both steps: it cross-compiles to wasm and extracts the embedded spec in one pass.

## Design

### 1. Installation

`scripts/fetch.sh` installs `groundhog-cli@0.2.1` user-wide:

```sh
cargo install --registry groundhog groundhog-cli@0.2.1
```

Without `--force`, this is a no-op if already installed.

### 2. Version pinning

`build_wasm.sh` asserts the installed version matches expectations:

```sh
EXPECTED="groundhog 0.2.1"
ACTUAL="$(groundhog --version)"
if [ "$ACTUAL" != "$EXPECTED" ]; then
    echo "error: expected '$EXPECTED', got '$ACTUAL'. Run scripts/fetch.sh" >&2
    exit 1
fi
```

### 3. build_wasm.sh

Rewrite internals to call `groundhog build` instead of raw cargo. The script keeps the same external interface (target name, build dir, flags) so CMakeLists.txt changes are minimal.

```sh
groundhog build -p "$TARGET_NAME" --json -- --frozen --offline
```

Output: the script copies `.wasm` and `.spec.json` from groundhog's output paths (reported in the JSON response) to `$TARGET_DIR/`.

When WASM_DEBUG_BACKTRACES is set, pass `-- -F logging` for debug features. Always pass `-F entrypoints`.

The `--optimize` flag replaces the separate wasm-opt step (remove `wasmopt_target()` calls from CMake).

### 4. Delete build_wasm_spec.sh

No longer needed. `groundhog build` produces the spec alongside the wasm.

### 5. generate_header.py — Rewrite for new spec format

#### New spec JSON schema (gh_contract_spec_v1)

```json
{
  "format": "gh_contract_spec_v1",
  "interfaces": [{
    "name": "Fibonacci",
    "version": "0.2.0",
    "throws": [{"selector": "0x...", "type": {"kind": "named", "name": "ERC20Error"}}],
    "methods": [{
      "name": "fibonacci",
      "args": [{"name": "n", "ty": {"kind": "u32"}}],
      "return_type": {"kind": "u32"}
    }]
  }],
  "type_defs": [{"kind": "enum", "name": "ERC20Error", "variants": [...]}]
}
```

#### Key mapping changes

| Old field | New field |
|-----------|-----------|
| `spec["interface"]` | `spec["interfaces"][0]["name"]` |
| `spec["version"]` | `spec["interfaces"][0]["version"]` |
| `spec["methods"]` | `spec["interfaces"][0]["methods"]` |
| `method["return"]["type"]` | `method["return_type"]["kind"]` |
| `method["args"][i]["typespec"]` | `method["args"][i]["ty"]` |
| `method["err"]` (per-method) | `interface["throws"]` (interface-level) |
| `method["entrypoint"]` | Computed (see below) |

#### Type kind mapping

| New `kind` | C++ type |
|------------|----------|
| `u32` | `uint32_t` |
| `i32` | `int32_t` |
| `u64` | `uint64_t` |
| `i64` | `int64_t` |
| `u128` | `std::tuple<uint64_t, uint64_t>` |
| `i128` | `std::tuple<int64_t, uint64_t>` |
| `bool` | `bool` |
| `void` | `void` |
| `address` | `ContractAddress` |
| `bytes` (with max) | `x23::Vec<uint8_t, max>` |
| `fixed_bytes` (with len) | `std::array<uint8_t, len>` |
| `named` (with name) | The named type |
| Structs/tuples | `std::tuple<...>` |

#### Entrypoint ID computation

Entrypoint IDs are no longer in the spec JSON. Compute them in Python:

```
entrypoint_id = sha256(interface_name + major_version + method_name)[0:8]
```

Major version extraction (matching `sdk_rs/contract-util/src/semver.rs`):
- `"1.2.3"` -> `"1"` (major > 0)
- `"0.9.2"` -> `"0.9"` (major == 0, minor > 0)
- `"0.0.2"` -> `"0.0.2"` (major == 0, minor == 0)

#### Error types

Error types move from per-method `err` to interface-level `throws`. The generated header uses the first throw type's name (if any) in the `Result<ReturnType, ErrorType>` wrapper for all methods on that interface.

### 6. Type defs to .x conversion

Add a new script `generate_types_xdr.py` (or fold into `generate_header.py`) that reads `type_defs` from the spec JSON and emits XDR `.x` format:

```json
{"kind": "enum", "name": "ERC20Error", "variants": [{"name": "AlreadyInitialized", "value": 1}]}
```

Produces:

```xdr
namespace itc_erc20 {
enum ERC20Error {
    AlreadyInitialized = 1,
};
} // namespace
```

The `.x` file then goes through x23c (`add_xdr()`) as before to produce `.hh`.

### 7. CMakeLists.txt changes

- **Remove** `add_spec_and_types_generation()` function and its invocation
- **Remove** `build_wasm_spec.sh` references
- **Remove** `wasmopt_target()` calls (wasm-opt handled by `--optimize`)
- **Modify** `add_wasm_builds()`: each contract's wasm build now also produces `_spec.json` as an output
- **Add** step to extract type_defs from spec JSON -> `.x` for TYPES_TARGETS
- **Modify** `add_contract_header()`: read spec JSON from wasm build output dir

### 8. What gets deleted

- `built_contracts/build_wasm_spec.sh`
- All `src/bin/generate_spec.rs` and `src/bin/generate_types.rs` in contracts (deferred cleanup)

Note: `built_contracts/.cargo/contract.toml` is required by `groundhog build` and must be kept.

### 9. What stays

- `build_wasm.sh` (rewritten internals, same CMake interface)
- `generate_header.py` (rewritten for new spec format)
- x23c pipeline for `.x` -> `.hh` conversion
- `Cargo.toml`, `Cargo.lock` at workspace level
