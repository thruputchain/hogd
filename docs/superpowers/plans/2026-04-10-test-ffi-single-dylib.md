# Consolidate test_ffi to Single Dylib — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate the second dylib (`libunified_lib`) by switching the Rust build back to a staticlib and linking it statically into `libtest_ffi`.

**Architecture:** Remove the conditional cdylib/staticlib toggle so `unified_lib` is always a staticlib. The existing symbol filtering on `test_ffi` (`-exported_symbols_list` on macOS, `--exclude-libs,ALL` on Linux) ensures Rust internals are not exported. Add dylib versioning via CMake `VERSION`/`SOVERSION`. Update CI to stop packaging the now-nonexistent second dylib.

**Tech Stack:** CMake, Rust/Cargo (via corrosion), GitHub Actions

**Spec:** `docs/superpowers/specs/2026-04-09-test-ffi-single-dylib-design.md`

---

### Task 1: Switch unified_lib to staticlib-only

**Files:**
- Modify: `hogd/rust/unified/Cargo.toml:20-21`
- Modify: `hogd/rust/CMakeLists.txt:17-23`

- [ ] **Step 1: Remove cdylib from Cargo.toml**

In `hogd/rust/unified/Cargo.toml`, change line 21 from:
```toml
crate-type = ["staticlib", "cdylib"]
```
To:
```toml
crate-type = ["staticlib"]
```

- [ ] **Step 2: Remove crate-type toggle from CMake**

In `hogd/rust/CMakeLists.txt`, replace lines 17-23:
```cmake
set(CRATE_TYPE "staticlib")
if (ENABLE_TEST_FFI)
  set(CRATE_TYPE "cdylib")
endif()

# Import targets defined in the workspace manifest `Cargo.toml` file
corrosion_import_crate(MANIFEST_PATH ./Cargo.toml CRATES unified_lib CRATE_TYPES ${CRATE_TYPE} LOCKED FROZEN)
```
With:
```cmake
# Import targets defined in the workspace manifest `Cargo.toml` file
corrosion_import_crate(MANIFEST_PATH ./Cargo.toml CRATES unified_lib CRATE_TYPES staticlib LOCKED FROZEN)
```

- [ ] **Step 3: Commit**

```bash
git add hogd/rust/unified/Cargo.toml hogd/rust/CMakeLists.txt
git commit -m "build: switch unified_lib to staticlib-only

Remove the conditional cdylib toggle. unified_lib is now always a
staticlib, linked into test_ffi at build time. Symbol filtering on
test_ffi prevents Rust internal symbol leakage."
```

---

### Task 2: Remove rpath and add dylib versioning to test_ffi

**Files:**
- Modify: `hogd/test_ffi/CMakeLists.txt:105-130`

- [ ] **Step 1: Remove rpath setup**

In `hogd/test_ffi/CMakeLists.txt`, delete lines 118-130 (the rpath block):
```cmake
# Set rpath so test_ffi can find the Rust cdylib in the same directory.
# Windows doesn't need this - it searches the DLL's directory by default.
if(APPLE)
  set_target_properties(test_ffi PROPERTIES
    BUILD_RPATH "@loader_path"
    INSTALL_RPATH "@loader_path"
  )
elseif(UNIX)
  set_target_properties(test_ffi PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
  )
endif()
```

- [ ] **Step 2: Add dylib version properties**

In `hogd/test_ffi/CMakeLists.txt`, after the symbol stripping block (after the `endif()` that closes the `APPLE`/`UNIX` symbol stripping at line 116), add:

```cmake
# Embed the test_ffi version into the shared library.
# macOS: sets Mach-O compatibility/current version fields.
# Linux: sets ELF soname and creates the .so symlink chain.
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/test_bindings.h _ver_lines
     REGEX "#define TEST_FFI_VERSION_(MAJOR|MINOR|PATCH)")
foreach(_line IN LISTS _ver_lines)
  if(_line MATCHES "VERSION_MAJOR +([0-9]+)")
    set(_ver_major ${CMAKE_MATCH_1})
  elseif(_line MATCHES "VERSION_MINOR +([0-9]+)")
    set(_ver_minor ${CMAKE_MATCH_1})
  elseif(_line MATCHES "VERSION_PATCH +([0-9]+)")
    set(_ver_patch ${CMAKE_MATCH_1})
  endif()
endforeach()
set_target_properties(test_ffi PROPERTIES
  VERSION "${_ver_major}.${_ver_minor}.${_ver_patch}"
  SOVERSION "${_ver_major}"
)
message(STATUS "test_ffi version: ${_ver_major}.${_ver_minor}.${_ver_patch}")
```

- [ ] **Step 3: Commit**

```bash
git add hogd/test_ffi/CMakeLists.txt
git commit -m "build: remove rpath, add version to test_ffi dylib

rpath was only needed to find the Rust cdylib at runtime. With
unified_lib as a staticlib, there is no separate dylib to find.

Also embed TEST_FFI_VERSION into the dylib via CMake VERSION/SOVERSION
so macOS compatibility/current version and Linux soname are set."
```

---

### Task 3: Update CI release workflow

**Files:**
- Modify: `.github/workflows/release_test_ffi.yml`

- [ ] **Step 1: Update linux-x86_64 job**

In the `build-linux-x86_64` job, replace the "Package Library" step (lines 89-96):
```yaml
      - name: Package Library
        run: |
          cp build/hogd/test_ffi/libtest_ffi.so release_artifacts/
          cp build/hogd/rust/libunified_lib.so release_artifacts/
          cp hogd/test_ffi/test_bindings.h release_artifacts/
          cd release_artifacts
          tar -czf libtest_ffi-linux-x86_64.tar.gz libtest_ffi.so libunified_lib.so test_bindings.h bindings.rs
          sha256sum libtest_ffi-linux-x86_64.tar.gz > libtest_ffi-linux-x86_64.tar.gz.sha256
```
With:
```yaml
      - name: Package Library
        run: |
          cp build/hogd/test_ffi/libtest_ffi.so release_artifacts/
          cp hogd/test_ffi/test_bindings.h release_artifacts/
          cd release_artifacts
          tar -czf libtest_ffi-linux-x86_64.tar.gz libtest_ffi.so test_bindings.h bindings.rs
          sha256sum libtest_ffi-linux-x86_64.tar.gz > libtest_ffi-linux-x86_64.tar.gz.sha256
```

- [ ] **Step 2: Update linux-arm64 job**

In the `build-linux-arm64` job, replace the "Package Library" step (lines 226-233):
```yaml
      - name: Package Library
        run: |
          cp build/hogd/test_ffi/libtest_ffi.so release_artifacts/
          cp build/hogd/rust/libunified_lib.so release_artifacts/
          cp hogd/test_ffi/test_bindings.h release_artifacts/
          cd release_artifacts
          tar -czf libtest_ffi-linux-arm64.tar.gz libtest_ffi.so libunified_lib.so test_bindings.h bindings.rs
          sha256sum libtest_ffi-linux-arm64.tar.gz > libtest_ffi-linux-arm64.tar.gz.sha256
```
With:
```yaml
      - name: Package Library
        run: |
          cp build/hogd/test_ffi/libtest_ffi.so release_artifacts/
          cp hogd/test_ffi/test_bindings.h release_artifacts/
          cd release_artifacts
          tar -czf libtest_ffi-linux-arm64.tar.gz libtest_ffi.so test_bindings.h bindings.rs
          sha256sum libtest_ffi-linux-arm64.tar.gz > libtest_ffi-linux-arm64.tar.gz.sha256
```

- [ ] **Step 3: Update macos-arm64 job**

In the `build-macos-arm64` job, replace the "Package Library" step (lines 369-376):
```yaml
      - name: Package Library
        run: |
          cp build/hogd/test_ffi/libtest_ffi.dylib release_artifacts/
          cp build/hogd/rust/libunified_lib.dylib release_artifacts/
          cp hogd/test_ffi/test_bindings.h release_artifacts/
          cd release_artifacts
          tar -czf libtest_ffi-macos-arm64.tar.gz libtest_ffi.dylib libunified_lib.dylib test_bindings.h bindings.rs
          shasum -a 256 libtest_ffi-macos-arm64.tar.gz > libtest_ffi-macos-arm64.tar.gz.sha256
```
With:
```yaml
      - name: Package Library
        run: |
          cp build/hogd/test_ffi/libtest_ffi.dylib release_artifacts/
          cp hogd/test_ffi/test_bindings.h release_artifacts/
          cd release_artifacts
          tar -czf libtest_ffi-macos-arm64.tar.gz libtest_ffi.dylib test_bindings.h bindings.rs
          shasum -a 256 libtest_ffi-macos-arm64.tar.gz > libtest_ffi-macos-arm64.tar.gz.sha256
```

- [ ] **Step 4: Update release notes template**

In the `create-release` job, replace the Contents section of the release body (lines 494-499):
```yaml
            ### Contents

            - `libtest_ffi.so` (Linux) or `libtest_ffi.dylib` (macOS) - Shared library
            - `libunified_lib.so` (Linux) or `libunified_lib.dylib` (macOS) - Shared library
            - `test_bindings.h` - C header file
            - `bindings.rs` - Pre-generated Rust bindings (no bindgen required)
```
With:
```yaml
            ### Contents

            - `libtest_ffi.so` (Linux) or `libtest_ffi.dylib` (macOS) - Shared library (Rust runtime statically linked)
            - `test_bindings.h` - C header file
            - `bindings.rs` - Pre-generated Rust bindings (no bindgen required)
```

- [ ] **Step 5: Commit**

```bash
git add .github/workflows/release_test_ffi.yml
git commit -m "ci: package single dylib in test_ffi release tarballs

unified_lib is now statically linked into test_ffi, so there is no
separate dylib to package. Remove libunified_lib from cp, tar, and
release notes in all three platform jobs."
```

---

### Task 4: Build and verify

- [ ] **Step 1: Configure a fresh build with test_ffi enabled**

```bash
./scripts/build.sh -d build_verify \
  -t Debug \
  -c clang \
  -x "-DENABLE_TEST_FFI=ON" \
  -n
```

- [ ] **Step 2: Build test_ffi target**

```bash
make -C build_verify -j$(sysctl -n hw.logicalcpu) test_ffi
```

Expected: build succeeds, no `libunified_lib.dylib` produced.

- [ ] **Step 3: Verify no unified_lib dylib exists**

```bash
ls build_verify/hogd/rust/libunified_lib* 2>&1
```

Expected: `No such file or directory`

- [ ] **Step 4: Verify test_ffi has no runtime dependency on unified_lib**

```bash
otool -L build_verify/hogd/test_ffi/libtest_ffi.dylib
```

Expected: output does NOT contain `unified_lib`. Should show system libs only.

- [ ] **Step 5: Verify version fields are populated**

```bash
otool -L build_verify/hogd/test_ffi/libtest_ffi.dylib | head -2
```

Expected: `libtest_ffi.0.dylib (compatibility version 0.0.0, current version 0.12.1)` (or similar — the key is it's not `0.0.0` for current version).

- [ ] **Step 6: Verify only the 29 C functions are exported**

```bash
nm -gU build_verify/hogd/test_ffi/libtest_ffi.dylib | grep -v '^\s*$' | wc -l
```

Expected: 29 (matching the count of symbols in `hogd/test_ffi/test_ffi.exports`).

- [ ] **Step 7: Run test_ffi_test**

```bash
make -C build_verify -j$(sysctl -n hw.logicalcpu) test_ffi_test
./build_verify/hogd/test_ffi/test_ffi_test
```

Expected: all tests pass.

- [ ] **Step 8: Run Rust workspace tests**

```bash
cd hogd/rust && cargo test --workspace --locked --frozen --offline
```

Expected: all tests pass (confirms `test_ffi_bindings` still works against the dylib).
