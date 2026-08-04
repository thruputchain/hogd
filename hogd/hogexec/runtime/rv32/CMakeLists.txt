find_package(OpenSSL REQUIRED)

# Portable RV32IM ISA core: instruction decode + execute, the register/memory
# model, and the Host boundary. This target intentionally has NO hogd link
# dependencies (no OpenSSL, no artifact/consensus layer, no runtime symbols) so
# it can be driven standalone by an external differential oracle (Sail/Spike).
# Keep it that way: anything needing OpenSSL, the artifact format, or hogd
# runtime symbols belongs in rv32_vm below, not here. rv32_isa_core_test
# mechanically enforces the boundary by linking only this target.
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

# Artifact container plus consensus digest/validation layered on top of the
# portable core. v4 moves method dispatch into the guest, so there is no
# host-side dispatch table.
add_library(
  rv32_vm
  artifact.cc
)

target_include_directories(
  rv32_vm
  PUBLIC
  ${CMAKE_SOURCE_DIR}
)

# x23 is for CHECK() in artifact.cc (hogd/base/log.h -> x23::panic): a digest
# that cannot be computed must stop the node, not become an artifact-level
# error. The dependency-narrow rule above binds rv32_isa_core, not this target.
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

# Links ONLY the portable core (no rv32_vm, no OpenSSL, no hogd runtime). Doubles
# as a smoke test for standalone decode/execute and as the mechanical guardrail
# for the core's hogd-free boundary: if the ISA core starts referencing a hogd
# symbol, this executable fails to link.
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
gtest_discover_tests(rv32_isa_core_test TEST_PREFIX rv32.isa_core.)

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
gtest_discover_tests(rv32_arch_trace_test TEST_PREFIX rv32.arch_trace.)

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
gtest_discover_tests(rv32_oracle_test TEST_PREFIX rv32.oracle.)

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
gtest_discover_tests(rv32_spike_oracle_test TEST_PREFIX rv32.spike_oracle.)

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
  rv32_conformance_test
  rv32_conformance_test.cc
  rv32_vm_test_main.cc
)

target_link_libraries(
  rv32_conformance_test
  rv32_vm
  GTest::gtest
)
gtest_discover_tests(
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
gtest_discover_tests(
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
gtest_discover_tests(
  rv32_spec_model_crosscheck_test
  TEST_PREFIX rv32.crosscheck.
  NO_PRETTY_VALUES
)

# Direct-threaded experimental interpreter (header-only threaded_*.h, requires a
# musttail attribute: clang, or GCC >= 15). Core-only: links rv32_isa_core ONLY
# because the differential test constructs the canonical Machine to diff
# against. Built unconditionally -- a toolchain without musttail hits the
# #error in musttail.h and fails the build, which is intended: the supported
# compilers all provide it, and a silent skip would hide a broken toolchain.
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
gtest_discover_tests(rv32_threaded_test TEST_PREFIX rv32.threaded.)

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
gtest_discover_tests(
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
gtest_discover_tests(rv32_syscall_bridge_test TEST_PREFIX rv32.bridge.)

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

# Compile the production rv32 objects with LTO so the final node link (the
# exec/bench targets already set INTERPROCEDURAL_OPTIMIZATION on themselves)
# can optimize across the interpreter/translation/syscall-bridge boundary.
# Without this, only rv32_vm_bench measured the interpreter under LTO while the
# node ran it without -- benchmark numbers didn't reflect production codegen.
#
# Gate on every optimized single-config build type, not the exact string
# "Release": RelWithDebInfo and MinSizeRel are optimized too, so the old
# exact-match silently dropped LTO for them (the very "production codegen" the
# comment above promises). check_ipo_supported() at the top level already
# guarantees IPO is available. Multi-config generators (Xcode/VS) leave
# CMAKE_BUILD_TYPE empty and are not covered here -- the benchmarks use
# single-config Make/Ninja.
if(CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
  set(HOGD_RV32_ENABLE_LTO TRUE)
endif()

if(HOGD_RV32_ENABLE_LTO)
  set_property(
    TARGET rv32_isa_core rv32_vm rv32_syscall_bridge rv32_runtime_objects
    PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE
  )
endif()

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
    rv32_syscall_bridge
    benchmark::benchmark
    built_headers_interface
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
    RV32_VM_BENCH_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
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
