# Standalone library for the native-contract-record helper (make_native_contract_record,
# make_deployed_contract_hash, default_wallet_deployed_contract_hash).  Both genesis and
# runtime need these symbols; genesis cannot link runtime (circular via db_interface), so
# they live in a small shared library with only protocol-level dependencies.
add_library(native_contract_record
  native_contract_record.cc
)
target_link_libraries(native_contract_record hogd_interface)
target_include_directories(native_contract_record PRIVATE ${CMAKE_BINARY_DIR})

add_library(item_objects OBJECT
  item_view.cc
  snapshot_storage_item.cc
  database_proxy.cc
  storage_snapshot_cache.cc
)
target_link_libraries(item_objects hogd_interface)
target_include_directories(item_objects PRIVATE ${CMAKE_BINARY_DIR})

# Standalone item tests — no syscall handlers, WASM runtimes, or precompiles
add_library(item_test_objects OBJECT
  item_view_test.cc
  snapshot_storage_item_test.cc
)
target_link_libraries(item_test_objects GTest::gtest hogd_interface)
target_include_directories(item_test_objects PRIVATE ${CMAKE_BINARY_DIR})

add_executable(item_test
  item_test_main.cc
  $<TARGET_OBJECTS:item_test_objects>
)
target_link_libraries(item_test
  runtime
  runtime_test_util
  hog_exec_common
  components
  protocol
  storage_cost
  GTest::gtest
)
gtest_discover_tests(item_test)
