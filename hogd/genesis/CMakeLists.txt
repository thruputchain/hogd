add_library(genesis
  genesis_block.cc
)

target_link_libraries(genesis
  base
  crypto
  storage_item
  protocol
  contract_loader
)

target_include_directories(genesis PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/hogd/xdr
  ${CMAKE_SOURCE_DIR}/built_contracts
)

# Add test objects
add_library(genesis_test_objects OBJECT
  genesis_block_test.cc
)

target_link_libraries(genesis_test_objects
  GTest::gtest_main
  crypto
)

target_include_directories(genesis_test_objects PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/hogd/xdr
  ${CMAKE_SOURCE_DIR}/built_contracts
)

# Create test executable
add_executable(genesis_test
  $<TARGET_OBJECTS:genesis_test_objects>
)

target_link_libraries(genesis_test
  genesis
  GTest::gtest_main
)

gtest_discover_tests(genesis_test)
