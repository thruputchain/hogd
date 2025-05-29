# Build hogexec modules
add_subdirectory(wasmi)
add_subdirectory(runtime)
add_subdirectory(runtime_trampoline)

# Build the frontend executor (hogexec) executable
add_executable(
  hogexec
  hogexec.cc
)
target_link_libraries(
  hogexec
  hogd
  runtime
)
