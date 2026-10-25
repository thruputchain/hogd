add_subdirectory(test_support)

# create base module library
add_library(
  base_objects OBJECT
  init.cc
  io.cc
  json.cc
  log.cc
  os.cc
  status.cc
  string.cc
)
target_link_libraries(
  base_objects
  built_headers_interface
  protocol
)
add_library(base STATIC $<TARGET_OBJECTS:base_objects>)

target_link_libraries(
  base
  x23
)

add_library(base_test_objects OBJECT
  arc_test.cc
  atomic_test.cc
  base_test.cc
  base58_test.cc
  bits_test.cc
  block_id_test.cc
  chunk_items_test.cc
  chunk_run_assembler_test.cc
  chunk_run_sender_test.cc
  chunked_vector_test.cc
  cli_test.cc
  cpputil_test.cc
  io_test.cc
  json_test.cc
  network_config_params_test.cc
  os_test.cc
  perthread_arc_test.cc
  perthread_test.cc
  rc_bytes_test.cc
  rc_test.cc
  status_test.cc
  string_test.cc
  time_test.cc
  unique_function_test.cc
  wide_integral_test.cc
  worker_pool_test.cc
  xdr_util_test.cc
)
target_link_libraries(
  base_test_objects
  GTest::gtest
  built_headers_interface
  protocol
)

# create base module test binary
add_executable(
  base_test
  $<TARGET_OBJECTS:base_test_objects>
)

target_link_libraries(
  base_test
  hogd
  GTest::gtest
)
hogd_discover_tests(base_test)
