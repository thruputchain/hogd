add_library(genesis
  genesis_block.cc
)

target_link_libraries(genesis
  base
  crypto
  storage_item
  protocol
  contract_loader
  runtime
)

target_include_directories(genesis PRIVATE
  ${CMAKE_BINARY_DIR}
  ${CMAKE_BINARY_DIR}/hogd/xdr
)

# Add test objects
add_library(genesis_test_objects OBJECT
  genesis_block_test.cc
)

target_link_libraries(genesis_test_objects
  GTest::gtest_main
  crypto
  runtime
  protocol
)

target_include_directories(genesis_test_objects PRIVATE
  ${CMAKE_BINARY_DIR}
  ${CMAKE_BINARY_DIR}/hogd/xdr
)

# Create test executable
add_executable(genesis_test
  $<TARGET_OBJECTS:genesis_test_objects>
)

target_link_libraries(genesis_test
  genesis
  runtime
  GTest::gtest_main
)

gtest_discover_tests(genesis_test)
