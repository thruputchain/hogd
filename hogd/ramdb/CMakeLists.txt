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
# A TIMEOUT property overrides ctest's --timeout rather than capping it, and
# ramdb is the only module here that sets one. At 100 it silently withheld the
# TSAN schedule's deliberately larger --timeout 300 from the heaviest tests in
# the tree, which is how InsertUntilOOM came to be killed at 100s under TSAN
# (#3584). Track that budget instead of undercutting it; the bound is kept so
# that the sanitizer runs in scripts/check.sh, which pass no --timeout at all,
# still fail a hung test in minutes rather than at ctest's 1500s default.
gtest_discover_tests(
  ramdb_test
  PROPERTIES TIMEOUT 300)
