find_package(OpenSSL REQUIRED)

# Standalone decode and canonical execution, without artifact or runtime link
# dependencies. rv32_isa_core_test checks this boundary by linking only the core.
add_library(
  rv32_isa_core
  machine.cc
  program.cc
)

target_include_directories(
  rv32_isa_core
  PUBLIC
  ${CMAKE_SOURCE_DIR}
)

# Artifact parsing and validation over the portable core.
add_library(
  rv32_vm
  artifact.cc
)

target_include_directories(
  rv32_vm
  PUBLIC
  ${CMAKE_SOURCE_DIR}
)

# x23 supplies CHECK(): a failed digest computation must stop the node.
target_link_libraries(
  rv32_vm
  PUBLIC
  rv32_isa_core
  PRIVATE
  OpenSSL::Crypto
  x23
)

add_library(
  rv32_syscall_bridge
  syscall_bridge.cc
)

add_custom_target(
  rv32_syscall_xdr_hh
)
add_dependencies(
  rv32_syscall_xdr_hh
  build_protocol
)

target_link_libraries(
  rv32_syscall_bridge
  rv32_vm
  protocol
)

target_include_directories(
  rv32_syscall_bridge
  PUBLIC
  ${CMAKE_SOURCE_DIR}
)

add_dependencies(
  rv32_syscall_bridge
  rv32_syscall_xdr_hh
)

# Standalone execution and link-dependency check.
add_executable(
  rv32_isa_core_test
  rv32_isa_core_test.cc
  rv32_vm_test_main.cc
)

target_link_libraries(
  rv32_isa_core_test
  rv32_isa_core
  GTest::gtest
)
hogd_discover_tests(rv32_isa_core_test TEST_PREFIX rv32.isa_core.)

# Differential-testing substrate (arch_trace.h). Also core-only: it must stay
# linkable against an external oracle harness with no hogd dependencies.
add_executable(
  rv32_arch_trace_test
  rv32_arch_trace_test.cc
  rv32_vm_test_main.cc
)

target_link_libraries(
  rv32_arch_trace_test
  rv32_isa_core
  GTest::gtest
)
hogd_discover_tests(rv32_arch_trace_test TEST_PREFIX rv32.arch_trace.)

# External-oracle differential harness (oracle.h + spec_model.h). Core-only for
# the same reason: the Oracle interface must stay linkable against an external
# reference model (Spike/sail-riscv) with no hogd dependencies.
add_executable(
  rv32_oracle_test
  rv32_oracle_test.cc
  rv32_vm_test_main.cc
)

target_link_libraries(
  rv32_oracle_test
  rv32_isa_core
  GTest::gtest
)
hogd_discover_tests(rv32_oracle_test TEST_PREFIX rv32.oracle.)

# External-oracle differential test (spike_oracle.h). Core-only; drives the real
# Spike (riscv-isa-sim) as a subprocess located via RV32_ORACLE_BIN and SKIPS
# when unset, so this adds no build-graph or default-CI dependency.
add_executable(
  rv32_spike_oracle_test
  rv32_spike_oracle_test.cc
  rv32_vm_test_main.cc
)

target_link_libraries(
  rv32_spike_oracle_test
  rv32_isa_core
  GTest::gtest
)
hogd_discover_tests(rv32_spike_oracle_test TEST_PREFIX rv32.spike_oracle.)

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
hogd_discover_tests(rv32_vm_test TEST_PREFIX rv32.)

add_executable(
  rv32_conformance_test
  rv32_conformance_test.cc
  rv32_vm_test_main.cc
)

target_link_libraries(
  rv32_conformance_test
  rv32_vm
  GTest::gtest
)
hogd_discover_tests(
  rv32_conformance_test
  TEST_PREFIX rv32.conformance.
  NO_PRETTY_VALUES
)

add_executable(
  rv32_borrowed_conformance_test
  rv32_borrowed_conformance_test.cc
  rv32_vm_test_main.cc
)

target_link_libraries(
  rv32_borrowed_conformance_test
  rv32_vm
  GTest::gtest
)
hogd_discover_tests(
  rv32_borrowed_conformance_test
  TEST_PREFIX rv32.borrowed.
  NO_PRETTY_VALUES
)

add_executable(
  rv32_spec_model_crosscheck_test
  rv32_spec_model_crosscheck_test.cc
  rv32_vm_test_main.cc
)

target_link_libraries(
  rv32_spec_model_crosscheck_test
  rv32_isa_core
  GTest::gtest
)
hogd_discover_tests(
  rv32_spec_model_crosscheck_test
  TEST_PREFIX rv32.crosscheck.
  NO_PRETTY_VALUES
)

# Differential tests for the production threaded interpreter. Links the
# canonical Machine through rv32_isa_core. Requires musttail (Clang or GCC 15+);
# musttail.h rejects unsupported compilers.
add_executable(
  rv32_threaded_test
  rv32_threaded_test.cc
  rv32_vm_test_main.cc
)

target_link_libraries(
  rv32_threaded_test
  rv32_isa_core
  GTest::gtest
)
hogd_discover_tests(rv32_threaded_test TEST_PREFIX rv32.threaded.)

add_executable(
  rv32_unsupported_profile_test
  rv32_unsupported_profile_test.cc
  rv32_vm_test_main.cc
)

target_link_libraries(
  rv32_unsupported_profile_test
  rv32_vm
  GTest::gtest
)
hogd_discover_tests(
  rv32_unsupported_profile_test
  TEST_PREFIX rv32.unsupported.
  NO_PRETTY_VALUES
)

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
hogd_discover_tests(rv32_syscall_bridge_test TEST_PREFIX rv32.bridge.)

add_library(rv32_runtime_objects OBJECT
  rv32_contract.cc
)

target_link_libraries(
  rv32_runtime_objects
  hogd_interface
  rv32_vm
)

target_include_directories(
  rv32_runtime_objects
  PRIVATE
  ${CMAKE_BINARY_DIR}/hogd/xdr
)

# LTO lets the final node link optimize across the interpreter, translation,
# and syscall bridge, matching benchmark code generation. Top-level IPO checks
# establish compiler support. This covers optimized single-config builds;
# multi-config generators leave CMAKE_BUILD_TYPE empty and are not covered.
if(CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
  set(HOGD_RV32_ENABLE_LTO TRUE)
endif()

if(HOGD_RV32_ENABLE_LTO)
  set_property(
    TARGET rv32_isa_core rv32_vm rv32_syscall_bridge rv32_runtime_objects
    PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE
  )
endif()

# Gas calibration harness. Added after HOGD_RV32_ENABLE_LTO is set above, which
# the subdirectory reads to build the harness with production codegen.
add_subdirectory(gas_calibration)

if(BUILD_BENCHMARKS)
  add_executable(
    rv32_vm_bench
    rv32_vm_bench.cc
  )

  target_link_libraries(
    rv32_vm_bench
    rv32_syscall_bridge
    benchmark::benchmark
  )

  target_include_directories(
    rv32_vm_bench
    PRIVATE
    ${CMAKE_BINARY_DIR}
  )

  if(HOGD_RV32_ENABLE_LTO)
    set_property(TARGET rv32_vm_bench PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
  endif()
endif()

if(BUILD_FUZZERS)
  add_library(
    rv32_isa_core_fuzz
    machine.cc
    program.cc
  )
  target_include_directories(
    rv32_isa_core_fuzz
    PUBLIC
    ${CMAKE_SOURCE_DIR}
  )
  target_compile_options(rv32_isa_core_fuzz PRIVATE
    -fsanitize=fuzzer-no-link,address)

  add_library(
    rv32_vm_fuzz
    artifact.cc
  )
  target_include_directories(
    rv32_vm_fuzz
    PUBLIC
    ${CMAKE_SOURCE_DIR}
  )
  target_link_libraries(
    rv32_vm_fuzz
    PUBLIC
    rv32_isa_core_fuzz
    PRIVATE
    OpenSSL::Crypto
    x23
  )
  target_compile_options(rv32_vm_fuzz PRIVATE
    -fsanitize=fuzzer-no-link,address)

  # Plain gtest for the pure carve/decoder-parity helpers (no fuzzer sanitizer).
  add_executable(
    rv32_fuzz_common_test
    EXCLUDE_FROM_ALL
    fuzz/fuzz_common_test.cc
    rv32_vm_test_main.cc
  )
  target_link_libraries(rv32_fuzz_common_test rv32_isa_core GTest::gtest)
  target_include_directories(rv32_fuzz_common_test PRIVATE ${CMAKE_SOURCE_DIR})

  add_executable(
    rv32_fuzz_engine_diff
    EXCLUDE_FROM_ALL
    fuzz/fuzz_engine_diff.cc
  )
  target_link_libraries(rv32_fuzz_engine_diff rv32_isa_core_fuzz)
  target_include_directories(rv32_fuzz_engine_diff PRIVATE ${CMAKE_SOURCE_DIR})
  target_compile_options(rv32_fuzz_engine_diff PRIVATE -fsanitize=fuzzer,address)
  target_link_options(rv32_fuzz_engine_diff PRIVATE
    -fsanitize=fuzzer,address -nostdlib++ -lc++)
  if(NOT APPLE)
    target_link_options(rv32_fuzz_engine_diff PRIVATE -lstdc++)
  endif()

  add_executable(
    rv32_fuzz_artifact
    EXCLUDE_FROM_ALL
    fuzz/fuzz_artifact.cc
  )
  target_link_libraries(rv32_fuzz_artifact rv32_vm_fuzz)
  target_include_directories(rv32_fuzz_artifact PRIVATE ${CMAKE_SOURCE_DIR})
  target_compile_options(rv32_fuzz_artifact PRIVATE -fsanitize=fuzzer,address)
  target_link_options(rv32_fuzz_artifact PRIVATE
    -fsanitize=fuzzer,address -nostdlib++ -lc++)
  if(NOT APPLE)
    target_link_options(rv32_fuzz_artifact PRIVATE -lstdc++)
  endif()

  add_executable(
    rv32_emit_seeds
    EXCLUDE_FROM_ALL
    fuzz/emit_seeds.cc
  )
  target_link_libraries(rv32_emit_seeds rv32_vm)
  target_include_directories(rv32_emit_seeds PRIVATE ${CMAKE_SOURCE_DIR})
endif()
