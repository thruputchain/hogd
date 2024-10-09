add_library(
  softstate
  hashset_deduplicate.cc
  atomic_hashset_node.cc
  delta_class_checker.cc
  storage_object_frame.cc
)

target_link_libraries(
  softstate
  protocol
  hogd
)

add_executable(
  softstate_test
  hashset_deduplicator_tests.cc
)

target_link_libraries(
  softstate_test
  softstate
  GTest::gtest_main
)
gtest_discover_tests(softstate_test)

# create crypto module benchmark binary
add_executable(
  softstate_bench
  softstate_bench.cc
)
target_link_libraries(
  softstate_bench
  softstate
  benchmark::benchmark
)
