add_library(
  rv32_vm
  machine.cc
  program.cc
)

target_include_directories(
  rv32_vm
  PUBLIC
  ${CMAKE_SOURCE_DIR}
)

add_executable(
  rv32_vm_test
  rv32_vm_test.cc
  rv32_vm_test_main.cc
)

target_link_libraries(
  rv32_vm_test
  rv32_vm
  GTest::gtest
)
gtest_discover_tests(rv32_vm_test TEST_PREFIX rv32.)

if(BUILD_BENCHMARKS)
  add_executable(
    rv32_vm_bench
    rv32_vm_bench.cc
  )

  target_link_libraries(
    rv32_vm_bench
    rv32_vm
    benchmark::benchmark
  )

  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set_property(TARGET rv32_vm_bench PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
  endif()
endif()
