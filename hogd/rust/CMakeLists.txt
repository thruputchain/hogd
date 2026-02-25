add_subdirectory(crypto)
add_subdirectory(wasmi)
add_subdirectory(wasmtime)
add_subdirectory(contract_types)

# include(cmake/check_rust_version.cmake)

# Check for sccache and configure it if available
find_program(SCCACHE_EXECUTABLE sccache)
if(SCCACHE_EXECUTABLE)
    message(STATUS "sccache found: ${SCCACHE_EXECUTABLE}")
else()
    message(STATUS "sccache not found - Rust builds will not use caching")
endif()

set(CRATE_TYPE "staticlib")
if (ENABLE_TEST_FFI)
  set(CRATE_TYPE "cdylib")
endif()

# Import targets defined in the workspace manifest `Cargo.toml` file
corrosion_import_crate(MANIFEST_PATH ./Cargo.toml CRATES unified_lib CRATE_TYPES ${CRATE_TYPE} LOCKED FROZEN)
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


# Sanitizer support (requires nightly toolchain + rust-src component).
# Enabled automatically when MSAN_LIBCXX_PATH is set (MSan build).
# Install prerequisites:
#   rustup install nightly
#   rustup component add rust-src --toolchain nightly
if(MSAN_LIBCXX_PATH)
  message(STATUS "Enabling MSan for Rust code (nightly + -Zbuild-std)")
  corrosion_add_target_rustflags(unified_lib
    "-Zsanitizer=memory"
    "-Zsanitizer-memory-track-origins=2"
  )
  # Recompile std with MSan instrumentation to avoid false positives.
  corrosion_set_cargo_flags(unified_lib -Zbuild-std=std,panic_abort)
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
