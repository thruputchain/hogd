# create container module test binary
add_executable(
  hex_array_test
  hex_array_test.cc
)
target_link_libraries(
  hex_array_test
  base
  hogd
  GTest::gtest_main
)
gtest_discover_tests(hex_array_test)

add_executable(
  merkle_trie_test
  merkle_trie_test.cc
)
target_link_libraries(
  merkle_trie_test
  base
  hogd
  GTest::gtest_main
)
gtest_discover_tests(merkle_trie_test)

add_executable(
  synchronized_map_test
  synchronized_map_test.cc
)
target_link_libraries(
  synchronized_map_test
  base
  GTest::gtest_main
)
gtest_discover_tests(synchronized_map_test)
