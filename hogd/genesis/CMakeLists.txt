add_library(genesis
  genesis_configuration.cc
)

add_dependencies(
  genesis
  all_wasm_headers
  generate_syscall_inner_h
  generate_syscall_meta_h
)

target_link_libraries(genesis
  native_contract_record
  base
  crypto
  storage_item
  protocol
  contract_loader
  components
)

target_include_directories(genesis PRIVATE
  ${CMAKE_BINARY_DIR}
  ${CMAKE_BINARY_DIR}/hogd/xdr
)

# Add test objects
add_library(genesis_test_objects OBJECT
  genesis_configuration_test.cc
  genesis_configuration_unfurl_tests.cc
)

target_link_libraries(genesis_test_objects
  GTest::gtest
  hogd_interface
)

target_include_directories(genesis_test_objects PRIVATE
  ${CMAKE_BINARY_DIR}
  ${CMAKE_BINARY_DIR}/hogd/xdr
)

add_dependencies(
  genesis_test_objects
  all_wasm_headers
  generate_syscall_inner_h
  generate_syscall_meta_h
)

# Create test executable
add_executable(genesis_test
  $<TARGET_OBJECTS:genesis_test_objects>
)

target_link_libraries(genesis_test
  genesis
  runtime
  runtime_test_lib
  precompiles
  GTest::gtest
)

gtest_discover_tests(genesis_test)
