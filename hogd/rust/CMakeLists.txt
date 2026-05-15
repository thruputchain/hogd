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
#set_property(TARGET unified_lib PROPERTY CARGO_BUILD_FLAGS "+nightly -Zsanitizer=address")

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
    # follow. -Zbuild-std rebuilds libstd with the same instrumentation;
    # without it, std's synchronization primitives stay precompiled-and-
    # unsanitized and TSAN false-flags any Rust-internal synchronization.
    message(STATUS "RUST_TSAN=ON: enabling -Zsanitizer=thread for Rust crates")
    corrosion_set_env_vars(unified_lib RUSTFLAGS=-Zsanitizer=thread)
    set_property(TARGET unified_lib APPEND PROPERTY CARGO_BUILD_FLAGS
        "-Zbuild-std=std,core,alloc,panic_unwind")
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