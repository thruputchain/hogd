add_subdirectory(crypto)
add_subdirectory(wasmi)
add_subdirectory(wasmtime)
add_subdirectory(wasm_meter)

# The version check enforces that the toolchain we configured matches
# the pin in rust-toolchain.toml. RUST_TSAN intentionally bypasses that
# pin to switch to nightly, so the check would fail in that mode.
if(NOT RUST_TSAN)
    include(cmake/check_rust_version.cmake)
else()
    message(STATUS
        "RUST_TSAN=ON: skipping rust-toolchain.toml version check "
        "(nightly toolchain override is intentional)")
endif()

# Check for sccache and configure it if available
find_program(SCCACHE_EXECUTABLE sccache)
if(SCCACHE_EXECUTABLE)
    message(STATUS "sccache found: ${SCCACHE_EXECUTABLE}")
else()
    message(STATUS "sccache not found - Rust builds will not use caching")
endif()

# Import targets defined in the workspace manifest `Cargo.toml` file
corrosion_import_crate(MANIFEST_PATH ./Cargo.toml CRATES unified_lib CRATE_TYPES staticlib LOCKED FROZEN)
# To enable an online build, set CARGO_NET_GIT_FETCH_WITH_CLI=true and remove CARGO_NET_OFFLINE=true
if(SCCACHE_EXECUTABLE)
    corrosion_set_env_vars(unified_lib CARGO_NET_OFFLINE=true RUSTC_WRAPPER=${SCCACHE_EXECUTABLE})
else()
    corrosion_set_env_vars(unified_lib CARGO_NET_OFFLINE=true)
endif()



corrosion_add_target_rustflags(unified_lib "-C target-cpu=${RUST_BUILD_CPU}")
message(STATUS "Rust target CPU: ${RUST_BUILD_CPU}")

# Build list of features to enable
set(FEATURES_TO_ENABLE "")

# Enable debug_logging if build type is Debug
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  list(APPEND FEATURES_TO_ENABLE debug_logging)
endif()

# Enable wasm_debug if WASM_DEBUG_BACKTRACES is set
if(WASM_DEBUG_BACKTRACES)
  list(APPEND FEATURES_TO_ENABLE wasm_debug)
endif()

# Apply features if any are enabled
if(FEATURES_TO_ENABLE)
  corrosion_set_features(unified_lib
      FEATURES
          ${FEATURES_TO_ENABLE}
  )
endif()


# If we want asan:
#
# Use nightly toolchain for Rust builds
#set(CARGO_BUILD_COMMAND "cargo +nightly build")
#set(CARGO_TEST_COMMAND "cargo +nightly test")
#set(CARGO_RUN_COMMAND "cargo +nightly run")

#corrosion_set_env_vars(unified_lib RUSTFLAGS="-Zsanitizer=address")
#corrosion_set_env_vars(unified_lib ASAN_OPTIONS=detect_stack_use_after_return=1:abort_on_error=1)
#corrosion_set_cargo_flags(unified_lib "-Zbuild-std=std,panic_abort")

if(RUST_TSAN)
    # The RUST_TSAN option is declared at top level; the toolchain
    # override to nightly happens there too (Rust_TOOLCHAIN must be set
    # before corrosion is loaded). Corrosion already passes --target on
    # every cargo invocation (Corrosion.cmake ~line 700), so we don't
    # need to inject one here — the host target is derived from the
    # nightly toolchain corrosion is configured with.
    #
    # -Zsanitizer=thread instruments Rust code so atomics, Mutex, and
    # parking_lot-style futex calls emit happens-before edges TSAN can
    # follow. -Zexternal-clangrt leaves the TSAN runtime to the C++ link,
    # which already uses -fsanitize=thread. The workspace profiles use
    # panic=abort, so rebuild libstd with panic_abort. Without -Zbuild-std,
    # std's synchronization primitives stay precompiled-and-unsanitized and
    # TSAN false-flags any Rust-internal synchronization.
    #
    # --cfg=rustix_use_libc forces the `rustix` crate onto its libc backend
    # instead of the Linux `linux_raw` backend it picks by default. wasmtime
    # mmaps/munmaps compiled-code objects through rustix; with the raw backend
    # those are inline `syscall` instructions that bypass TSAN's libc munmap
    # interceptor, so freeing a code mmap never clears its shadow. When the
    # kernel later hands the same virtual address to another worker thread's
    # fresh compilation, TSAN reports a stale write/write race in
    # finish_object (no happens-before across the unseen munmap/mmap reuse) —
    # a false positive with no real shared memory. Routing through libc lets
    # the interceptor clear shadow on free, eliminating the false positive
    # while preserving detection of genuine races on that path.
    message(STATUS "RUST_TSAN=ON: enabling Rust ThreadSanitizer instrumentation")
    corrosion_add_target_rustflags(unified_lib
        "-Zsanitizer=thread"
        "-Zexternal-clangrt"
        "--cfg=rustix_use_libc")
    corrosion_set_cargo_flags(unified_lib
        "-Zbuild-std=std,panic_abort")
endif()

# Add cargo tests to ctest
add_test(
  NAME rust_workspace_tests
  COMMAND cargo test --workspace --locked --frozen --offline
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

# Set environment variables for the test
set_tests_properties(rust_workspace_tests PROPERTIES
  ENVIRONMENT "CARGO_NET_OFFLINE=true"
)
