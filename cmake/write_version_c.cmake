include("${SOURCE_DIR}/cmake/repro.cmake")

# This script is invoked from a build target, not during CMake configure.
# That lets us re-sample git state on each build invocation instead of keeping
# whatever commit ids were visible the last time CMake generated the build tree.
#
# In particular, the protocol repo is a submodule, so its checked-out commit can
# change without any edit to the top-level CMake files. Doing the lookup here
# ensures version.c tracks that submodule move automatically.
git_describe_string(GIT_HOGD_COMMIT "${SOURCE_DIR}")
git_describe_string(GIT_PROTOCOL_COMMIT "${SOURCE_DIR}/protocol" NODIRTY)

# Load the template source and substitute the values we just computed into the
# @VAR@ placeholders used by cmake/version.c.in.
file(READ "${VERSION_TEMPLATE}" version_template)
string(CONFIGURE "${version_template}" version_content @ONLY)

# The build may start from a clean directory, so create the parent folder for
# the generated source before attempting to write the file.
get_filename_component(version_output_dir "${VERSION_OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${version_output_dir}")

# Default to writing the file, then suppress the write if the rendered content
# already matches the existing output exactly.
set(write_output TRUE)
if(EXISTS "${VERSION_OUTPUT}")
  file(READ "${VERSION_OUTPUT}" existing_content)
  # Preserve the file timestamp on no-op builds so dependents do not recompile.
  if(existing_content STREQUAL version_content)
    set(write_output FALSE)
  endif()
endif()

# Only touch version.c when the embedded strings actually changed.
if(write_output)
  file(WRITE "${VERSION_OUTPUT}" "${version_content}")
endif()
