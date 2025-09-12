add_library(
  merkle_objects OBJECT
  node.cc
  merkle_trie.cc
)

target_link_libraries(
  merkle_objects
  hogd_interface
)

add_library(
  merkle
  $<TARGET_OBJECTS:merkle_objects>
)

target_link_libraries(
  merkle
  hogd
)

add_executable(
  hex_array_test
  hex_array_test.cc
)
target_link_libraries(
  hex_array_test
  hogd
  GTest::gtest_main
)
gtest_discover_tests(hex_array_test)

add_executable(
  node_test
  node_test.cc
)
target_link_libraries(
  node_test
  hogd
  merkle
  GTest::gtest_main
)
gtest_discover_tests(node_test)

add_executable(
  merkle_trie_test
  merkle_trie_test.cc
)
target_link_libraries(
  merkle_trie_test
  base
  hogd
  merkle
  GTest::gtest_main
)
gtest_discover_tests(merkle_trie_test)

add_executable(
  merkle_trie_bench
  merkle_trie_bench.cc
)
target_link_libraries(
  merkle_trie_bench
  merkle
  benchmark::benchmark
)
