add_library(
  syscall
  syscall_entry.cc
)

target_link_libraries(
  syscall
  protocol
)
