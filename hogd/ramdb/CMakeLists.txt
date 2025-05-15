# Create a ramdb module library
add_library(
  ramdb
  db.cc
  db_status.cc
  disk_reader.cc
  disk_writer.cc
  item_allocator.cc
  loglet.cc
  scalable_map.cc
)
target_link_libraries(
  ramdb
  hogd
  crypto
)

# Build the db_bench executable
add_executable(
  db_bench
  db_bench.cc
)
target_link_libraries(
  db_bench
  ramdb
)

# create test binary
add_executable(
  db_test
  db_test.cc
)
target_link_libraries(
  db_test
  ramdb
  GTest::gtest_main
)
gtest_discover_tests(db_test)
