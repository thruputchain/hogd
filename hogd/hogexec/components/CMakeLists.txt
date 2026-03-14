add_library(components_objects OBJECT
  execution_mode.cc
  hash_buffers.cc
  ${CMAKE_SOURCE_DIR}/hogd/hogexec/runtime/contract_trait.cc
)
target_link_libraries(components_objects hogd_interface)
target_include_directories(components_objects PRIVATE ${CMAKE_BINARY_DIR})
add_library(components STATIC $<TARGET_OBJECTS:components_objects>)
target_link_libraries(components crypto hogexec_common)

# Own test executable (same pattern as runtime_test)
add_library(components_test_objects OBJECT
  alias_guard_test.cc
  execution_mode_test.cc
  hash_buffers_test.cc
)
target_link_libraries(components_test_objects GTest::gtest hogd_interface)
target_include_directories(components_test_objects PRIVATE ${CMAKE_BINARY_DIR})

add_executable(components_test $<TARGET_OBJECTS:components_test_objects>)
target_link_libraries(components_test
  components
  hogexec_common
  hogd
  GTest::gtest
  GTest::gtest_main
)
gtest_discover_tests(components_test)

if(BUILD_BENCHMARKS)
  add_executable(
    hash_buffers_bench
    hash_buffers_bench.cc
  )
  target_link_libraries(
    hash_buffers_bench
    components
    hogexec_common
    hogd
    benchmark::benchmark
    benchmark::benchmark_main
  )

  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set_property(TARGET hash_buffers_bench PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
  endif()
endif()
