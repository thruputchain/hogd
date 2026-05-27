add_library(
  rv32_vm
  artifact.cc
  machine.cc
  program.cc
)

target_include_directories(
  rv32_vm
  PUBLIC
  ${CMAKE_SOURCE_DIR}
)

add_library(
  rv32_syscall_bridge
  syscall_bridge.cc
)

add_custom_target(
  rv32_syscall_xdr_hh
  DEPENDS
  ${CMAKE_BINARY_DIR}/hogd/xdr/ffi_enums.hh
  ${CMAKE_BINARY_DIR}/hogd/xdr/syscall_error.hh
)

target_link_libraries(
  rv32_syscall_bridge
  rv32_vm
)

target_include_directories(
  rv32_syscall_bridge
  PUBLIC
  ${CMAKE_SOURCE_DIR}
  PRIVATE
  ${CMAKE_BINARY_DIR}/hogd/xdr
)

add_dependencies(
  rv32_syscall_bridge
  rv32_syscall_xdr_hh
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

add_executable(
  rv32_syscall_bridge_test
  rv32_syscall_bridge_test.cc
  rv32_vm_test_main.cc
)

target_link_libraries(
  rv32_syscall_bridge_test
  rv32_syscall_bridge
  x23
  GTest::gtest
)

target_include_directories(
  rv32_syscall_bridge_test
  PRIVATE
  ${CMAKE_BINARY_DIR}/hogd/xdr
)

add_dependencies(
  rv32_syscall_bridge_test
  rv32_syscall_xdr_hh
)
gtest_discover_tests(rv32_syscall_bridge_test TEST_PREFIX rv32.bridge.)

if(BUILD_BENCHMARKS)
  wat2wasm_target(
    rv32_fibonacci_bench_wasm
    fibonacci_bench.wat
    ${CMAKE_CURRENT_BINARY_DIR}/rv32_fibonacci_bench.wasm
  )

  add_executable(
    rv32_vm_bench
    rv32_vm_bench.cc
  )

  target_link_libraries(
    rv32_vm_bench
    rv32_vm
    benchmark::benchmark
    unified_lib
  )

  target_include_directories(
    rv32_vm_bench
    PRIVATE
    ${CMAKE_BINARY_DIR}
  )

  add_dependencies(
    rv32_vm_bench
    rv32_fibonacci_bench_wasm
    all_wasms_built
    generate_wasmi_ffi_h
  )

  target_compile_definitions(
    rv32_vm_bench
    PRIVATE
    CMAKE_ROOT_BINARY_DIR="${CMAKE_BINARY_DIR}"
    RV32_VM_BENCH_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
  )

  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set_property(TARGET rv32_vm_bench PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
  endif()
endif()
