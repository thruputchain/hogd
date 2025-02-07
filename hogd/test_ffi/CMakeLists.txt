add_library(
    test_ffi
    SHARED
    test_bindings.cc
)

target_link_libraries(
  test_ffi
  hogdb_core
  hogexec_core
)
