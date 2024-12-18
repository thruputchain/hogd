add_library(
  runtime_trampoline
  ffi_trampolines.cc
)

target_link_libraries(
  runtime_trampoline
  protocol
)

add_dependencies(
  runtime_trampoline
  protocol
)
