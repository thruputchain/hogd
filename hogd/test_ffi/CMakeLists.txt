add_library(
    test_ffi
    test_bindings.cc)

target_link_libraries(
  test_ffi
  hogexec_core
  hogdb_core
  coord_core
)
