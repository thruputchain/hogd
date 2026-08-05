# Build hog_exec modules
# Note: wasmi is now built via the main rust workspace
add_subdirectory(common)
add_subdirectory(components)
add_subdirectory(runtime)
add_subdirectory(interface)
add_subdirectory(bench)
add_subdirectory(precompiles)
add_subdirectory(contract_tests)

# Build the frontend executor (hog_exec) executable
add_executable(
  hog-exec
  hog_exec.cc
)
target_link_libraries(
  hog-exec
  exec_interface
  metrics
  http
  hogd
  runtime
  precompiles
)
