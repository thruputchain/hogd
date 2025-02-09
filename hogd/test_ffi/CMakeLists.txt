set_source_files_properties(
    ${CMAKE_BINARY_DIR}/hogd/hogexec/runtime/syscall_inner.h
    PROPERTIES GENERATED TRUE)

add_library(
    test_ffi
    SHARED
    test_bindings.cc
)

add_dependencies(
  test_ffi
  generate_syscall_inner_h
)

target_include_directories(
  test_ffi
  PRIVATE
  ${CMAKE_BINARY_DIR}
)

target_link_libraries(
  test_ffi
  hogdb_core
  hogexec_core
)
