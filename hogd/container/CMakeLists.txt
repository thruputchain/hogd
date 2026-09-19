add_subdirectory(merkle)

# create container module test binary
add_executable(
  container_test
  flat_key_set_test.cc
  synchronized_map_test.cc
  concurrent_map_test.cc
  concurrent_set_test.cc
)
target_link_libraries(
  container_test
  base
  hogd
  GTest::gtest
)
gtest_discover_tests(container_test)

if(BUILD_BENCHMARKS)
  add_executable(
    container_bench
    container_bench.cc
  )
  target_link_libraries(
    container_bench
    hogd
    benchmark::benchmark
  )

  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set_property(TARGET container_bench PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
  endif()
endif()
