# Build hogexec modules
add_subdirectory(wasmi)
add_subdirectory(runtime)
add_subdirectory(runtime_trampoline)
add_subdirectory(interface)
add_subdirectory(bench)
add_subdirectory(precompiles)

# Build the frontend executor (hogexec) executable
add_executable(
  hogexec
  hogexec.cc
)
target_link_libraries(
  hogexec
  hogd
  runtime
  precompiles
)
