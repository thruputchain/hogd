# Inputs:
#  - SRC_DIR: source dir with the .git repo
#  - OUT_FILE: path to write the generated header
#  - PROJECT_VERSION: (optional) from project()

# Get git info (works even if not in a git checkout)
find_program(GIT_EXEC git)
set(GIT_HOGD_HASH "unknown")

set(PROTOCOL_SRC_DIR "${SRC_DIR}/protocol" CACHE STRING "protocol repository path")
set(GIT_PROTOCOL_HASH "unknown")

if(GIT_EXEC)
  # hogd.
  execute_process(
    COMMAND "${GIT_EXEC}" -C "${SRC_DIR}" rev-parse --short=12 HEAD
    OUTPUT_VARIABLE GIT_HOGD_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )

  # protocol.
  execute_process(
    COMMAND "${GIT_EXEC}" -C "${PROTOCOL_SRC_DIR}" rev-parse --short=12 HEAD
    OUTPUT_VARIABLE GIT_PROTOCOL_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
endif()

# Compose the header text
set(_txt "/* Auto-generated. Do not edit. */

namespace hogd::version {

extern const char *const kHogdVersion           = \"${PROJECT_VERSION}\";
extern const char *const kHogdGitHash           = \"${GIT_HOGD_HASH}\";
extern const char *const kHogdProtocol          = \"hogp/${GIT_PROTOCOL_HASH}\";

} // namespace hogd::version 
")

# Write only if changed (prevents gratuitous recompiles)
file(WRITE "${OUT_FILE}.tmp" "${_txt}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${OUT_FILE}.tmp" "${OUT_FILE}")
file(REMOVE "${OUT_FILE}.tmp")
