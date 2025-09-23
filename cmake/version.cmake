# Inputs:
#  - SRC_DIR: source dir with the .git repo
#  - OUT_FILE: path to write the generated header
#  - PROJECT_VERSION: (optional) from project()

message("hi!")
# Get git info (works even if not in a git checkout)
find_program(GIT_EXEC git)
set(GIT_HOGD_HASH "unknown")
set(GIT_HOGD_DIRTY "")

set(PROTOCOL_SRC_DIR "${SRC_DIR}/protocol" CACHE STRING "protocol repository path")
set(GIT_PROTOCOL_HASH "unknown")
set(GIT_PROTOCOL_DIRTY "")

if(GIT_EXEC)
  # hogd.
  execute_process(
    COMMAND "${GIT_EXEC}" -C "${SRC_DIR}" rev-parse --short=12 HEAD
    OUTPUT_VARIABLE GIT_HOGD_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  execute_process(
    COMMAND "${GIT_EXEC}" -C "${SRC_DIR}" status --porcelain
    OUTPUT_VARIABLE GIT_HOGD_STATUS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  if(GIT_HOGD_STATUS)
    set(GIT_HOGD_DIRTY "+dirty")
  endif()

  # protocol.
  execute_process(
    COMMAND "${GIT_EXEC}" -C "${PROTOCOL_SRC_DIR}" rev-parse --short=12 HEAD
    OUTPUT_VARIABLE GIT_PROTOCOL_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  execute_process(
    COMMAND "${GIT_EXEC}" -C "${PROTOCOL_SRC_DIR}" status --porcelain
    OUTPUT_VARIABLE GIT_PROTOCOL_STATUS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  if(GIT_PROTOCOL_STATUS)
    set(GIT_PROTOCOL_DIRTY "+dirty")
  endif()
endif()

# Build timestamp (UTC) so you can see when the binary was produced
string(TIMESTAMP BUILD_TIME "%Y-%m-%dT%H:%M:%SZ" UTC)

message("${GIT_HOGD_HASH}${GIT_HOGD_DIRTY}")

# Compose the header text
set(_txt "/* Auto-generated. Do not edit. */

#pragma once

namespace hogd::version {

constexpr const char *kHogdVersion           = \"${PROJECT_VERSION}\";
constexpr const char *kHogdGitHash           = \"${GIT_HOGD_HASH}\";
constexpr const char *kHogdGitHashFull       = \"${GIT_HOGD_HASH}${GIT_HOGD_DIRTY}\";
constexpr const char *kHogdBuildTime         = \"${BUILD_TIME}\";

constexpr const char *kHogdProtocolHash      = \"${GIT_PROTOCOL_HASH}\";
constexpr const char *kHogdProtocolHashFull  = \"${GIT_PROTOCOL_HASH}${GIT_PROTOCOL_DIRTY}\";
constexpr const char *kHogdProtocol          = \"hogp/${GIT_PROTOCOL_HASH}${GIT_PROTOCOL_DIRTY}\";

} // namespace hogd::version 
")

# Write only if changed (prevents gratuitous recompiles)
file(WRITE "${OUT_FILE}.tmp" "${_txt}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${OUT_FILE}.tmp" "${OUT_FILE}")
file(REMOVE "${OUT_FILE}.tmp")
