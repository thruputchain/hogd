set_source_files_properties(
    ${CMAKE_CURRENT_BINARY_DIR}/mock_syscall_inner.h
    PROPERTIES GENERATED TRUE)

set_source_files_properties(
    ${CMAKE_CURRENT_BINARY_DIR}/mock_syscall_entry.cc
    PROPERTIES GENERATED TRUE)

set_source_files_properties(
    ${CMAKE_CURRENT_BINARY_DIR}/mock_syscall_inner.cc
    PROPERTIES GENERATED TRUE)

add_library(
    test_ffi
    SHARED
    test_bindings.cc
    db_context_wrapper.cc
    mock_syscall_entry.cc
    mock_syscall_inner.cc
)

add_dependencies(
  test_ffi
  generate_mock_syscall_inner_h
  generate_mock_syscall_inner_cc
  generate_mock_syscall_entry_cc
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
