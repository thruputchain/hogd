# groundhog-cli Build Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the raw-cargo contract build pipeline with `groundhog build` from `groundhog-cli`, eliminating the separate spec build step.

**Architecture:** `groundhog build -p <name>` replaces both `build_wasm.sh` (wasm cross-compilation) and `build_wasm_spec.sh` (spec generation). The `build_wasm.sh` script is rewritten to wrap `groundhog build`. `generate_header.py` is rewritten for the new `gh_contract_spec_v1` JSON format. A new `generate_types_xdr.py` converts spec type_defs to `.x` for the existing x23c pipeline.

**Tech Stack:** Shell (build_wasm.sh), Python (generate_header.py, generate_types_xdr.py), CMake (CMakeLists.txt), groundhog-cli 0.2.1

---

## File Map

| File | Action | Responsibility |
|------|--------|----------------|
| `scripts/fetch.sh` | Modify | Add `cargo install groundhog-cli@0.2.1` |
| `built_contracts/build_wasm.sh` | Rewrite | Wrap `groundhog build`, copy outputs, version check |
| `built_contracts/build_wasm_spec.sh` | Delete | Replaced by groundhog build |
| `built_contracts/generate_header.py` | Rewrite | Parse `gh_contract_spec_v1`, compute entrypoints, map types |
| `built_contracts/generate_types_xdr.py` | Create | Convert spec JSON type_defs to `.x` format |
| `built_contracts/CMakeLists.txt` | Rewrite | Remove spec pipeline, remove wasm-opt, wire new flow |

---

### Task 1: Update fetch.sh

**Files:**
- Modify: `scripts/fetch.sh`

- [ ] **Step 1: Add groundhog-cli install to fetch.sh**

At the end of `scripts/fetch.sh`, add:

```sh
echo "Installing groundhog-cli..."
cargo install --registry groundhog groundhog-cli@0.2.1
```

- [ ] **Step 2: Verify it works**

Run: `./scripts/fetch.sh`
Expected: prints "Installing groundhog-cli..." then either installs or says already installed. Exit 0.

- [ ] **Step 3: Commit**

```bash
git add scripts/fetch.sh
git commit -m "build: install groundhog-cli via fetch.sh"
```

---

### Task 2: Rewrite build_wasm.sh

**Files:**
- Rewrite: `built_contracts/build_wasm.sh`

The script keeps its existing interface (`build_wasm.sh <target_name> <target_dir> [flags...]`) but internally calls `groundhog build`.

The current CMakeLists.txt calls it as:
```
build_wasm.sh <target_name> <build_dir> [--release] [-F features]
```

The new script needs to:
1. Assert groundhog version
2. Call `groundhog build -p <target> --json [--optimize] -- --frozen --offline [-F features]`
3. Parse JSON output to get wasm and spec paths
4. Copy wasm to `<build_dir>/<target>.wasm` and spec to `<build_dir>/<target>_spec.json`

- [ ] **Step 1: Write the new build_wasm.sh**

```sh
#!/bin/sh
# Build a WASM contract via groundhog-cli
# Usage: build_wasm.sh <target_name> <target_dir> [additional cargo flags...]
#
# Produces:
#   <target_dir>/<target_name>.wasm
#   <target_dir>/<target_name>_spec.json

set -e

# Version gate
EXPECTED_VERSION="groundhog 0.2.1"
ACTUAL_VERSION="$(groundhog --version 2>/dev/null || true)"
if [ "$ACTUAL_VERSION" != "$EXPECTED_VERSION" ]; then
    echo "error: expected '$EXPECTED_VERSION', got '${ACTUAL_VERSION:-not installed}'. Run scripts/fetch.sh" >&2
    exit 1
fi

TARGET_NAME="$1"
TARGET_DIR="$2"

if [ -z "$TARGET_NAME" ] || [ -z "$TARGET_DIR" ]; then
    echo "Usage: build_wasm.sh <target_name> <target_dir> [additional cargo flags...]" >&2
    exit 1
fi

shift 2

# Clean environment
unset RUSTFLAGS
unset CARGO_ENCODED_RUSTFLAGS
export CARGO_NET_GIT_FETCH_WITH_CLI=true

# Build with groundhog-cli
# Pass remaining args (e.g. --release, -F features) through to cargo
OUTPUT=$(groundhog build -p "${TARGET_NAME}" --optimize --json -- --frozen --offline "$@")

# Parse output paths from JSON
WASM_PATH=$(echo "$OUTPUT" | jq -r '.data.wasm')
SPEC_PATH=$(echo "$OUTPUT" | jq -r '.data.spec_json')

if [ -z "$WASM_PATH" ] || [ "$WASM_PATH" = "null" ]; then
    echo "error: groundhog build failed for ${TARGET_NAME}" >&2
    echo "$OUTPUT" | jq -r '.error.message // empty' >&2
    exit 1
fi

# Copy outputs to build directory
mkdir -p "${TARGET_DIR}"
cp "${WASM_PATH}" "${TARGET_DIR}/${TARGET_NAME}.wasm"
cp "${SPEC_PATH}" "${TARGET_DIR}/${TARGET_NAME}_spec.json"
```

- [ ] **Step 2: Test manually on fibonacci**

Run from `built_contracts/`:
```bash
./build_wasm.sh fibonacci /tmp/test_build -F entrypoints
```
Expected: `/tmp/test_build/fibonacci.wasm` and `/tmp/test_build/fibonacci_spec.json` both exist. The spec JSON has `format: "gh_contract_spec_v1"`.

- [ ] **Step 3: Commit**

```bash
git add built_contracts/build_wasm.sh
git commit -m "build: rewrite build_wasm.sh to use groundhog-cli"
```

---

### Task 3: Rewrite generate_header.py for new spec format

**Files:**
- Rewrite: `built_contracts/generate_header.py`

The script's CLI interface stays the same (`generate_header.py <spec.json> -o <output.h> -n <namespace> -w <wasm_file> [-i include]`). The internals change to parse `gh_contract_spec_v1`.

- [ ] **Step 1: Write the new generate_header.py**

The complete rewritten script. Key changes:
- Parse `interfaces[0]` instead of top-level fields
- New `map_type()` function for `kind`-based type mapping
- Compute entrypoint IDs via `sha256(interface_name + major_version + method_name)[0:8]`
- Error types from `interface.throws` (interface-level, not per-method)

```python
#!/usr/bin/env python3

import argparse
import hashlib
import json
import os
import struct
import sys


def major_version(version_str):
    """Extract major version matching sdk_rs/contract-util/src/semver.rs.

    - "1.2.3" -> "1"
    - "0.9.2" -> "0.9"
    - "0.0.2" -> "0.0.2"
    """
    parts = version_str.split(".")
    major = int(parts[0]) if len(parts) > 0 else 0
    minor = int(parts[1]) if len(parts) > 1 else 0
    patch = int(parts[2].split("-")[0]) if len(parts) > 2 else 0
    if major > 0:
        return str(major)
    elif minor > 0:
        return f"{major}.{minor}"
    elif patch > 0:
        return f"{major}.{minor}.{patch}"
    else:
        return version_str


def compute_entrypoint_id(interface_name, version_str, method_name):
    """Compute entrypoint ID: sha256(interface_name || major_ver || method_name)[0:8]."""
    mv = major_version(version_str)
    h = hashlib.sha256()
    h.update(interface_name.encode())
    h.update(mv.encode())
    h.update(method_name.encode())
    return struct.unpack(">Q", h.digest()[:8])[0]


def map_type(ty):
    """Map a spec type descriptor to a C++ type string."""
    kind = ty["kind"]
    if kind == "u8":
        return "uint8_t"
    elif kind == "u32":
        return "uint32_t"
    elif kind == "i32":
        return "int32_t"
    elif kind == "u64":
        return "uint64_t"
    elif kind == "i64":
        return "int64_t"
    elif kind == "u128":
        return "std::tuple<uint64_t, uint64_t>"
    elif kind == "i128":
        return "std::tuple<int64_t, uint64_t>"
    elif kind == "bool":
        return "bool"
    elif kind == "void":
        return "void"
    elif kind == "address":
        return "ContractAddress"
    elif kind == "vec":
        inner = map_type(ty["inner"])
        max_len = ty["max_len"]
        return f"x23::Vec<{inner}, {max_len}>"
    elif kind == "array":
        inner = map_type(ty["element"])
        length = ty["len"]
        return f"std::array<{inner}, {length}>"
    elif kind == "option":
        inner = map_type(ty["inner"])
        return f"x23::Ptr<{inner}>"
    elif kind == "tuple":
        items = [map_type(item) for item in ty["items"]]
        return f"std::tuple<{', '.join(items)}>"
    elif kind == "named":
        return ty["name"]
    else:
        return kind


def generate_header(
    json_file, output_file=None, namespace=None, extra_includes=None, wasm_file=None
):
    with open(json_file, "r") as f:
        spec = json.load(f)

    interface = spec["interfaces"][0]
    interface_name = interface["name"]
    version = interface["version"]
    methods = interface["methods"]
    throws = interface.get("throws", [])

    # Error type: use first throw type name if any
    err_type = None
    if throws:
        err_ty = throws[0].get("type", {})
        if err_ty.get("kind") == "named":
            err_type = err_ty["name"]

    # Method names and entrypoint IDs
    export_names = [m["name"] for m in methods]
    entrypoints = [
        compute_entrypoint_id(interface_name, version, name) for name in export_names
    ]

    export_names_str = ",\n            ".join(f'"{name}"' for name in export_names)
    entrypoints_str = ",\n            ".join(f"0x{ep:016x}ULL" for ep in entrypoints)

    # Function signatures
    function_signatures = []
    for method in methods:
        return_type = map_type(method["return_type"])
        args = [map_type(arg["ty"]) for arg in method["args"]]

        if err_type:
            return_type = f"Result<{return_type}, {err_type}>"

        if args:
            sig = f"{return_type}(*)({', '.join(args)})"
        else:
            sig = f"{return_type}(*)()"
        function_signatures.append(sig)

    signatures_str = ",\n             ".join(function_signatures)

    namespace_forward = f"namespace {namespace} {{\n" if namespace else ""
    namespace_close = f"\n}} // namespace {namespace}" if namespace else ""
    wasm_str = (
        f'constexpr static std::string_view kWasmFile = "{wasm_file}";'
        if wasm_file
        else ""
    )

    if extra_includes:
        extra_includes_str = "\n".join(
            f'#include "{include}"' for include in extra_includes
        )
    else:
        extra_includes_str = ""

    header_content = f"""#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <tuple>

#include "x23/vec.h"
#include "x23/x23.h"
#include "types.hh"
#include "hogd/hogexec/runtime/contract_result.h"

// Auto-generated from contract spec
// DO NOT EDIT

using namespace hogd;

{extra_includes_str}

{namespace_forward}

struct {interface_name} {{
    constexpr static std::string_view kTraitName = "{interface_name}";
    constexpr static std::string_view kVersion = "{version}";
    {wasm_str}

    constexpr static std::string_view kSerializationFormat = "cxs";

    // These will be sorted before being passed as indices to method id derivation.
    constexpr static auto kExportNames = std::array<std::string_view, {len(export_names)}>{{
            {export_names_str}
        }};

    constexpr static auto kEntrypoints = std::array<uint64_t, {len(entrypoints)}>{{
            {entrypoints_str}
        }};

    using export_types = std::tuple<{signatures_str}>;
}};

{namespace_close}

"""

    if output_file:
        with open(output_file, "w") as f:
            f.write(header_content)
        print(f"Generated header file: {output_file}")
    else:
        print(header_content)


def main():
    parser = argparse.ArgumentParser(
        description="Generate a C++ header file from a JSON contract spec.",
    )
    parser.add_argument("json_spec_file", help="Path to the JSON contract spec file")
    parser.add_argument("-o", "--output", dest="output_file")
    parser.add_argument("-n", "--namespace")
    parser.add_argument("-w", "--wasm", dest="wasm_file")
    parser.add_argument("-i", "--include", dest="extra_includes", action="append")
    args = parser.parse_args()

    if not os.path.exists(args.json_spec_file):
        print(f"Error: JSON spec file '{args.json_spec_file}' not found", file=sys.stderr)
        sys.exit(1)

    generate_header(
        args.json_spec_file,
        args.output_file,
        args.namespace,
        args.extra_includes,
        args.wasm_file,
    )


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Test against fibonacci spec**

```bash
python3 built_contracts/generate_header.py \
    built_contracts/target/wasm32-unknown-unknown/release/fibonacci.spec.json \
    -n fibonacci -w fibonacci.wasm
```

Expected: printed header matches structure of old `vscode/built_contracts/fibonacci.h`. Specifically:
- `kTraitName = "Fibonacci"`, `kVersion = "0.2.0"`
- 4 methods: burn_gas, change_nonce_config, fibonacci, slow_fibonacci
- Entrypoint hex values match old header (0x0625524778880d03, etc.)
- Type signatures: `void(*)()`, `void(*)(uint32_t)`, `uint32_t(*)(uint32_t)`, `uint32_t(*)(uint32_t)`

- [ ] **Step 3: Test against itc_erc20 spec (has error types)**

```bash
python3 built_contracts/generate_header.py \
    built_contracts/target/wasm32-unknown-unknown/release/itc_erc20.spec.json \
    -n itc_erc20 -w itc_erc20.wasm
```

Expected: all method return types wrapped in `Result<..., ERC20Error>`.

- [ ] **Step 4: Commit**

```bash
git add built_contracts/generate_header.py
git commit -m "build: rewrite generate_header.py for gh_contract_spec_v1"
```

---

### Task 4: Create generate_types_xdr.py

**Files:**
- Create: `built_contracts/generate_types_xdr.py`

Reads `type_defs` from a spec JSON and emits XDR `.x` format. The namespace is passed as an argument.

- [ ] **Step 1: Write generate_types_xdr.py**

```python
#!/usr/bin/env python3
"""Convert spec JSON type_defs to XDR .x format."""

import argparse
import json
import sys


def emit_enum(name, variants):
    lines = [f"enum {name} {{"]
    for i, v in enumerate(variants):
        comma = "," if i < len(variants) - 1 else ""
        lines.append(f"\t{v['name']} = {v['value']}{comma}")
    lines.append("};")
    return "\n".join(lines)


def emit_type_def(td):
    kind = td["kind"]
    if kind == "enum":
        return emit_enum(td["name"], td["variants"])
    else:
        print(f"warning: unsupported type_def kind '{kind}', skipping", file=sys.stderr)
        return None


def main():
    parser = argparse.ArgumentParser(description="Generate .x from spec JSON type_defs")
    parser.add_argument("spec_json", help="Path to spec JSON file")
    parser.add_argument("-o", "--output", required=True, help="Output .x file")
    parser.add_argument("-n", "--namespace", required=True, help="XDR namespace")
    args = parser.parse_args()

    with open(args.spec_json) as f:
        spec = json.load(f)

    type_defs = spec.get("type_defs", [])
    if not type_defs:
        # Write empty file with namespace
        with open(args.output, "w") as f:
            f.write(f"namespace {args.namespace} {{\n\n}} // namespace\n")
        return

    body_parts = []
    for td in type_defs:
        result = emit_type_def(td)
        if result:
            body_parts.append(result)

    with open(args.output, "w") as f:
        f.write(f"namespace {args.namespace} {{\n\n")
        f.write("\n\n".join(body_parts))
        f.write(f"\n\n\n}} // namespace\n")

    print(f"Generated {args.output}")


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Test against itc_erc20 spec**

```bash
python3 built_contracts/generate_types_xdr.py \
    built_contracts/target/wasm32-unknown-unknown/release/itc_erc20.spec.json \
    -o /tmp/itc_erc20.x -n itc_erc20
cat /tmp/itc_erc20.x
```

Expected output:
```
namespace itc_erc20 {

enum ERC20Error {
	AlreadyInitialized = 1,
	MustBeContractOwner = 2,
	TooLarge = 3,
	Internal = 100
};


} // namespace
```

Compare with `vscode/built_contracts/itc_erc20.x` — should match structurally.

- [ ] **Step 3: Test against fibonacci spec (no type_defs)**

```bash
python3 built_contracts/generate_types_xdr.py \
    built_contracts/target/wasm32-unknown-unknown/release/fibonacci.spec.json \
    -o /tmp/fibonacci.x -n fibonacci
cat /tmp/fibonacci.x
```

Expected: empty namespace file.

- [ ] **Step 4: Commit**

```bash
git add built_contracts/generate_types_xdr.py
git commit -m "build: add generate_types_xdr.py for spec type_defs to .x conversion"
```

---

### Task 5: Rewrite CMakeLists.txt

**Files:**
- Rewrite: `built_contracts/CMakeLists.txt`

This is the largest change. The new flow:
1. `add_wasm_builds()` calls `build_wasm.sh` per contract, producing both `.wasm` and `_spec.json`
2. No more `add_spec_and_types_generation()` — specs come from step 1
3. For TYPES_TARGETS, a new step runs `generate_types_xdr.py` on the spec JSON to produce `.x`
4. `add_contract_header()` depends on the wasm build (which produces the spec), not on `all_spec_and_types`
5. No more `wasmopt_target()` calls — `--optimize` in build_wasm.sh handles it

- [ ] **Step 1: Rewrite the CMakeLists.txt**

Replace the entire file. Key structural changes:

**Remove:**
- `WASM_BUILD_FOLDER`, `WASM_CARGO_RELEASE_FLAG`, `WASMOPT_DEBUG_FLAG` variables
- `add_wasm_builds()` function (replaced with new version)
- `add_spec_and_types_generation()` function and its invocation
- All `wasmopt_target()` calls
- `all_unoptimized_wasms` / `all_optimized_wasms` targets
- `spec_build` directory references

**New `add_wasm_builds()`:**
```cmake
function(add_wasm_builds)
  set(common_depends
    ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.lock
    ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml
    ${roundtrip_rs_stamp})

  set(all_wasms "")
  set(all_specs "")
  set(all_depends ${common_depends})
  set(build_commands "")

  foreach(target_name ${ARGN})
    list(APPEND all_wasms ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.wasm)
    list(APPEND all_specs ${CMAKE_CURRENT_BINARY_DIR}/${target_name}_spec.json)

    file(GLOB target_rs_files
      ${CMAKE_CURRENT_SOURCE_DIR}/integration_test_contracts/${target_name}/src/*.rs)
    list(FILTER target_rs_files EXCLUDE REGEX "roundtrip\\.rs$")

    list(APPEND all_depends
      ${CMAKE_CURRENT_SOURCE_DIR}/integration_test_contracts/${target_name}/Cargo.toml
      ${target_rs_files})

    list(APPEND build_commands
      COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/build_wasm.sh
        ${target_name}
        ${CMAKE_CURRENT_BINARY_DIR}
        "${WASM_FLAGS}")
  endforeach()

  add_custom_command(
    OUTPUT ${all_wasms} ${all_specs}
    ${build_commands}
    DEPENDS ${all_depends}
    COMMENT "Building all WASM contracts via groundhog-cli"
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  )

  add_custom_target(all_wasms_built DEPENDS ${all_wasms} ${all_specs})
  add_dependencies(all_wasm_contracts all_wasms_built)
endfunction()
```

**New `add_types_xdr()` function** for TYPES_TARGETS:
```cmake
function(add_types_xdr target_name)
  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.x
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/generate_types_xdr.py
        ${CMAKE_CURRENT_BINARY_DIR}/${target_name}_spec.json
        -o ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.x
        -n ${target_name}
    DEPENDS
      ${CMAKE_CURRENT_SOURCE_DIR}/generate_types_xdr.py
      ${CMAKE_CURRENT_BINARY_DIR}/${target_name}_spec.json
    COMMENT "Generating ${target_name}.x from spec type_defs"
  )
endfunction()
```

**Updated `add_contract_header()`:** Change dependency from `all_spec_and_types` to `all_wasms_built`:
```cmake
add_dependencies(${target_name}_header all_wasms_built)
```

**WASM_FLAGS simplification** — only need the feature flags now, no release/debug folder distinction:
```cmake
if(WASM_DEBUG_BACKTRACES)
  set(WASM_FLAGS "-F logging,entrypoints")
  message(STATUS "WASM contracts: debug build with backtraces enabled")
else()
  set(WASM_FLAGS "-F entrypoints")
  message(STATUS "WASM contracts: release build (minimal size, no backtraces)")
endif()
```

**Types targets wiring** — replace the `add_spec_and_types_generation(TYPES_TARGETS ...)` block with:
```cmake
# Generate .x files from spec JSON type_defs
add_types_xdr(itc_erc20)
add_types_xdr(manual_revert_test)
add_types_xdr(txs_roundtrip)
add_types_xdr(cxs_roundtrip)
add_types_xdr(payment_channel)

# XDR .x -> .hh via x23c (existing add_xdr function)
add_xdr(itc_erc20_hh ./itc_erc20.x)
add_xdr(manual_revert_test_hh ./manual_revert_test.x)
add_xdr(txs_roundtrip_hh ./txs_roundtrip.x)
add_xdr(cxs_roundtrip_hh ./cxs_roundtrip.x)
add_xdr(payment_channel_hh ./payment_channel.x)

# Ensure XDR gen waits for spec JSONs
add_dependencies(build_itc_erc20_hh all_wasms_built)
add_dependencies(build_manual_revert_test_hh all_wasms_built)
add_dependencies(build_txs_roundtrip_hh all_wasms_built)
add_dependencies(build_cxs_roundtrip_hh all_wasms_built)
add_dependencies(build_payment_channel_hh all_wasms_built)
```

**Bottom target:** Replace `add_dependencies(build_all_contracts all_wasm_contracts all_spec_and_types)` with:
```cmake
add_custom_target(build_all_contracts ALL)
add_dependencies(build_all_contracts all_wasm_contracts)
```

- [ ] **Step 2: Delete build_wasm_spec.sh**

```bash
rm built_contracts/build_wasm_spec.sh
```

- [ ] **Step 3: Test the build**

```bash
cmake --build vscode --target all_wasm_contracts -j8
```

Expected: all contracts build, spec JSONs and headers are generated. Compare a few generated headers against old ones to verify correctness (especially entrypoint IDs and type signatures).

- [ ] **Step 4: Verify specific headers match old output**

Check fibonacci.h entrypoints match the old values:
```bash
grep "0x0625524778880d03\|0x03538676eda289c1\|0x1b487165f3068bac\|0xcf64be945e401647" vscode/built_contracts/fibonacci.h
```
Expected: all 4 values present.

Check itc_erc20.h has `Result<..., ERC20Error>`:
```bash
grep "Result<" vscode/built_contracts/itc_erc20.h | head -3
```
Expected: `Result<void, ERC20Error>` and `Result<std::tuple<...>, ERC20Error>`.

- [ ] **Step 5: Commit**

```bash
git add built_contracts/CMakeLists.txt
git rm built_contracts/build_wasm_spec.sh
git commit -m "build: migrate CMake to groundhog-cli, remove spec build pipeline"
```

---

### Task 6: Clean up and verify full build

**Files:**
- Modify: `built_contracts/CMakeLists.txt` (if needed)

- [ ] **Step 1: Run full cmake reconfigure and build**

```bash
cd vscode && cmake . && cmake --build . --target all_wasm_contracts -j8
```

Expected: clean build with no errors.

- [ ] **Step 2: Run the C++ tests that depend on generated headers**

```bash
cmake --build vscode --target test_ffi_test -j8 && cd vscode && ctest -R test_ffi -j4
```

Expected: builds and tests pass (except known failures from manual_revert_test incompatibility).

- [ ] **Step 3: Commit any fixups**

If any adjustments were needed, commit them.

- [ ] **Step 4: Push and verify CI**

```bash
git push
```

Monitor CI checks.
