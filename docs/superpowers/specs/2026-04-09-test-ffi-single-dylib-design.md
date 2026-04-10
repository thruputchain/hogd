# Consolidate test_ffi to a Single Dylib

## Problem

The test_ffi build currently produces two dynamic libraries:

- `libtest_ffi.{dylib,so}` — C++ code (mock runtime, syscall handlers, FFI bindings)
- `libunified_lib.{dylib,so}` — Rust code (crypto, wasmi, wasmtime)

`test_ffi` dynamically links `unified_lib` at runtime via rpath (`@loader_path` / `$ORIGIN`). Both dylibs must be present together, both are packaged in CI release tarballs, and consumers (`groundhog-sys`) must have both on the library search path.

This two-dylib setup exists because of a historical workaround: when `unified_lib` was a staticlib, its Rust internal symbols (allocator, panic runtime, stdlib internals) leaked through `test_ffi.dylib`'s export table, causing conflicts when loaded by Rust consumers built with a different toolchain version.

## Why This Is Now Safe

Symbol filtering was added to `test_ffi` after the cdylib workaround:

- **macOS**: `-exported_symbols_list` restricts exports to the 29 C functions in `test_ffi.exports`
- **Linux**: `--exclude-libs,ALL` hides symbols from static archives; C++ symbols are hidden via `CXX_VISIBILITY_PRESET hidden`

With this filtering in place, Rust internal symbols from a statically linked `unified_lib` will be present in the binary but **not exported**. No toolchain version conflicts.

## Design

### 1. Rust crate-type: always staticlib

**`hogd/rust/unified/Cargo.toml`**

Change:
```toml
crate-type = ["staticlib", "cdylib"]
```
To:
```toml
crate-type = ["staticlib"]
```

**`hogd/rust/CMakeLists.txt`**

Remove the conditional crate-type toggle (lines 17-20):
```cmake
set(CRATE_TYPE "staticlib")
if (ENABLE_TEST_FFI)
  set(CRATE_TYPE "cdylib")
endif()
```

Replace with a hardcoded staticlib in the `corrosion_import_crate` call:
```cmake
corrosion_import_crate(MANIFEST_PATH ./Cargo.toml CRATES unified_lib CRATE_TYPES staticlib LOCKED FROZEN)
```

### 2. test_ffi CMakeLists.txt

**Remove rpath setup** (lines 118-130): The `@loader_path` / `$ORIGIN` rpath configuration existed solely so `test_ffi` could find `unified_lib.dylib` at runtime. With a staticlib, there is no separate dylib to find.

**Add dylib version**: Parse `TEST_FFI_VERSION_{MAJOR,MINOR,PATCH}` from `test_bindings.h` at CMake configure time and set `VERSION` / `SOVERSION` on the `test_ffi` target:

- **macOS**: Populates the Mach-O `compatibility version` / `current version` fields (currently `0.0.0`)
- **Linux**: Sets the ELF soname and creates the standard `libtest_ffi.so -> libtest_ffi.so.0 -> libtest_ffi.so.0.12.1` symlink chain

**No changes to**: symbol filtering (`-exported_symbols_list`, `--exclude-libs,ALL`), static libc++ linking, OBJECT library, SHARED library definition, test executable.

### 3. CI release workflow

**`.github/workflows/release_test_ffi.yml`**: In all three build jobs (linux-x86_64, linux-arm64, macos-arm64):

- Remove the `cp build/hogd/rust/libunified_lib.{so,dylib} release_artifacts/` line
- Remove `libunified_lib.{so,dylib}` from the `tar` command
- Update the release notes template "Contents" section to remove `libunified_lib` references

Tarball contents go from `{libtest_ffi, libunified_lib, test_bindings.h, bindings.rs}` to `{libtest_ffi, test_bindings.h, bindings.rs}`.

### 4. No changes to groundhog-sys

`groundhog-sys/build.rs` already only links `test_ffi` (`cargo:rustc-link-lib=dylib=test_ffi`). It never referenced `unified_lib`. The runtime simplification (one dylib instead of two) is transparent to the consumer.

## Files Changed

| File | Change |
|------|--------|
| `hogd/rust/unified/Cargo.toml` | `crate-type = ["staticlib"]` (drop `"cdylib"`) |
| `hogd/rust/CMakeLists.txt` | Remove crate-type toggle, hardcode `staticlib` |
| `hogd/test_ffi/CMakeLists.txt` | Remove rpath setup, add `VERSION`/`SOVERSION` |
| `.github/workflows/release_test_ffi.yml` | Remove `libunified_lib` from cp/tar/release notes (3 jobs) |

## Files NOT Changed

- `hogd/test_ffi/test_ffi.exports` — unchanged, still the authoritative symbol list
- `hogd/test_ffi/*.cc` — no source changes
- `hogd/test_ffi/test_bindings.h` — unchanged
- `sdk_rs/groundhog-sys/` — no changes needed
- `hogd/crypto/CMakeLists.txt`, `hogd/hogexec/runtime/CMakeLists.txt` — these link `unified_lib` as a CMake target; CMake resolves it as a static archive, no change needed

## Verification

- Build with `ENABLE_TEST_FFI=ON`, confirm only `libtest_ffi.{dylib,so}` is produced (no `libunified_lib`)
- `otool -L` (macOS) / `ldd` (Linux) confirms no runtime dependency on `unified_lib`
- `otool -L` confirms version fields are populated (not `0.0.0`)
- `nm -gU` (macOS) / `nm -D` (Linux) confirms only the 29 C functions from `test_ffi.exports` are exported
- Run `test_ffi_test` to confirm the test executable still works
- Run Rust workspace tests (`cargo test --workspace`) to confirm `test_ffi_bindings` still works
