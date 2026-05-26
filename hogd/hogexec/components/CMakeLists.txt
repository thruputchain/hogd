add_xdr(
  components_library_exception
  library_exception.x
)
add_dependencies(build_components_library_exception protocol)

add_library(components_objects OBJECT
  auth_token_registry.cc
  execution_mode.cc
  hash_buffers.cc
  ${CMAKE_SOURCE_DIR}/hogd/hogexec/runtime/contract_trait.cc
)
target_link_libraries(
  components_objects
  hogd_interface
  components_library_exception
)
target_include_directories(components_objects PRIVATE ${CMAKE_BINARY_DIR})
add_library(components STATIC $<TARGET_OBJECTS:components_objects>)
target_link_libraries(components crypto hogexec_common)

# Own test executable (same pattern as runtime_test)
add_library(components_test_objects OBJECT
  alias_guard_test.cc
  auth_token_registry_test.cc
  borrowed_contract_batch_test.cc
  execution_mode_test.cc
  ffi_helpers_tests.cc
  hash_buffers_test.cc
)
target_link_libraries(
  components_test_objects
  GTest::gtest
  hogd_interface
  components_library_exception
)
target_include_directories(components_test_objects PRIVATE ${CMAKE_BINARY_DIR})

add_executable(components_test $<TARGET_OBJECTS:components_test_objects>)
target_link_libraries(
  components_test
  components
  components_library_exception
  hogexec_common
  hogd
  GTest::gtest
)
gtest_discover_tests(components_test)
