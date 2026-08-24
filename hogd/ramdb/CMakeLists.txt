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
  metrics
)

if(BUILD_BENCHMARKS)
  # Build the db_bench executable
  add_executable(
    ramdb_bench
    db_bench.cc
  )
  target_link_libraries(
    ramdb_bench
    ramdb
  )
endif()

# Build the db_crash_test executable
add_executable(
  db_crash_test
  db_crash_test.cc
)
target_link_libraries(
  db_crash_test
  ramdb
)

# create test binary
add_executable(
  ramdb_test
  db_test.cc
)
target_link_libraries(
  ramdb_test
  ramdb
  GTest::gtest_main
)
# Deliberately no TIMEOUT property: it would override each invocation's
# --timeout rather than cap it, so pinning one here hands every schedule the
# same budget. A 100 pinned here is what withheld the TSAN schedule's
# --timeout 300 from InsertUntilOOM and got it killed at 100s (#3584). Every
# ctest invocation that runs these tests passes its own --timeout instead.
gtest_discover_tests(ramdb_test)
