# Create a ramdb module library
add_library(ramdb_objects OBJECT
  db.cc
  db_status.cc
  disk_reader.cc
  disk_writer.cc
  item_allocator.cc
  loglet.cc
  scalable_map.cc
)
target_link_libraries(
  ramdb_objects
  hogd_interface
)

add_library(
  ramdb
  $<TARGET_OBJECTS:ramdb_objects>
)

target_link_libraries(
  ramdb
  hogd
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

# Build the db_crash_test executable
add_executable(
  db_crash_test
  db_crash_test.cc
)
target_link_libraries(
  db_crash_test
  ramdb
  metrics
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
gtest_discover_tests(
  db_test
  PROPERTIES TIMEOUT 100)
