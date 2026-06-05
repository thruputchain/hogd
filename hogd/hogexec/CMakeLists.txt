# Build hogexec modules
# Note: wasmi is now built via the main rust workspace
add_subdirectory(common)
add_subdirectory(components)
add_subdirectory(runtime)
add_subdirectory(interface)
add_subdirectory(bench)
add_subdirectory(precompiles)
add_subdirectory(contract_tests)

# Build the frontend executor (hogexec) executable
add_executable(
  hogexec
  hogexec.cc
)
target_link_libraries(
  hogexec
  exec_interface
  health
  http
  hogd
  runtime
  precompiles
)
