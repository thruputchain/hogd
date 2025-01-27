# Build hogexec modules
add_subdirectory(runtime_trampoline)
add_subdirectory(wasmi)
add_subdirectory(runtime)
add_subdirectory(interface)

add_library(
  hogexec_core
  hogexec_core.cc
)

target_link_libraries(
  hogexec_core
  hogd
  protocol
  runtime
  interface
)

# Build the frontend executor (hogexec) executable
add_executable(
  hogexec
  hogexec.cc
)
target_link_libraries(
  hogexec
  hogexec_core
)
