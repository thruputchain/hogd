# Check that Rust_VERSION matches the channel in rust-toolchain.toml
# This script should be included after Corrosion has been set up and Rust_VERSION is available

# Find rust-toolchain.toml relative to this script's location
# This script is in hogd/rust/cmake/, so rust-toolchain.toml is in hogd/rust/
get_filename_component(SCRIPT_DIR "${CMAKE_CURRENT_LIST_FILE}" DIRECTORY)
set(RUST_TOOLCHAIN_TOML "${SCRIPT_DIR}/../rust-toolchain.toml")
get_filename_component(RUST_TOOLCHAIN_TOML "${RUST_TOOLCHAIN_TOML}" ABSOLUTE)

if(NOT EXISTS "${RUST_TOOLCHAIN_TOML}")
    message(FATAL_ERROR "rust-toolchain.toml not found at ${RUST_TOOLCHAIN_TOML}")
endif()

# Read the rust-toolchain.toml file
file(READ "${RUST_TOOLCHAIN_TOML}" RUST_TOOLCHAIN_CONTENT)

# Extract the channel value using regex
# Pattern: channel = "version" or channel = 'version'
# Try with double quotes first
string(REGEX MATCH "channel[ \t]*=[ \t]*\"([^\"]+)\"" _ "${RUST_TOOLCHAIN_CONTENT}")
if(CMAKE_MATCH_1)
    set(RUST_TOOLCHAIN_CHANNEL "${CMAKE_MATCH_1}")
else()
    # Try with single quotes
    string(REGEX MATCH "channel[ \t]*=[ \t]*'([^']+)'" _ "${RUST_TOOLCHAIN_CONTENT}")
    if(CMAKE_MATCH_1)
        set(RUST_TOOLCHAIN_CHANNEL "${CMAKE_MATCH_1}")
    else()
        message(FATAL_ERROR "Could not parse channel from rust-toolchain.toml\nFile content:\n${RUST_TOOLCHAIN_CONTENT}")
    endif()
endif()

# Normalize versions for comparison (strip any trailing metadata like -nightly, etc.)
# Extract just the version number part (e.g., "1.90.0" from "1.90.0" or "1.90.0-nightly")
string(REGEX MATCH "^([0-9]+\\.[0-9]+\\.[0-9]+)" RUST_TOOLCHAIN_VERSION "${RUST_TOOLCHAIN_CHANNEL}")
if(NOT RUST_TOOLCHAIN_VERSION)
    # If it doesn't match the pattern, use the whole channel (might be "stable", "nightly", etc.)
    set(RUST_TOOLCHAIN_VERSION "${RUST_TOOLCHAIN_CHANNEL}")
endif()

# Compare versions
if(NOT DEFINED Rust_VERSION)
    message(FATAL_ERROR "Rust_VERSION is not defined. Make sure Corrosion is set up before including this script.")
endif()

# Normalize Rust_VERSION similarly
string(REGEX MATCH "^([0-9]+\\.[0-9]+\\.[0-9]+)" RUST_VERSION_NORMALIZED "${Rust_VERSION}")
if(NOT RUST_VERSION_NORMALIZED)
    set(RUST_VERSION_NORMALIZED "${Rust_VERSION}")
endif()

# Check if versions match
if(NOT "${RUST_TOOLCHAIN_VERSION}" STREQUAL "${RUST_VERSION_NORMALIZED}")
    message(FATAL_ERROR
        "Rust version mismatch!\n"
        "  rust-toolchain.toml channel: ${RUST_TOOLCHAIN_CHANNEL} (parsed as: ${RUST_TOOLCHAIN_VERSION})\n"
        "  CMake Rust_VERSION: ${Rust_VERSION} (normalized as: ${RUST_VERSION_NORMALIZED})\n"
        "  These must match. Please update either rust-toolchain.toml or Rust_TOOLCHAIN in CMakeLists.txt"
    )
endif()

message(STATUS "Rust version check passed: ${RUST_VERSION_NORMALIZED} matches rust-toolchain.toml")

