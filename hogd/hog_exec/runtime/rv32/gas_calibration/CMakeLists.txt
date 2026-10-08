# ISA-only calibration: no artifact, protocol, or syscall runtime dependencies.
# scripts/rv32_gas_calibrate.sh builds these targets through CMake in Release.

add_library(
  rv32_gas_calibration
  kernel.cc
  lowering_audit.cc
  measure.cc
  provenance.cc
  syscall_kernel.cc
  workloads.cc
)

target_link_libraries(
  rv32_gas_calibration
  rv32_isa_core
)

target_include_directories(
  rv32_gas_calibration
  PUBLIC
  ${CMAKE_SOURCE_DIR}
)

# Record the build and hash interpreter sources. The behavioural fingerprint
# misses handler/layout changes; the source digest detects them but also moves
# on comment edits, so a mismatch is a review signal rather than a timing result.
set(HOGD_RV32_ENGINE_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/../threaded_machine.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../threaded_program.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../isa_ops.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../program.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../program.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/../machine.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../machine.cc
)
set(HOGD_RV32_ENGINE_DIGEST_INPUT "")
foreach(engine_source ${HOGD_RV32_ENGINE_SOURCES})
  file(SHA256 "${engine_source}" engine_source_hash)
  string(APPEND HOGD_RV32_ENGINE_DIGEST_INPUT "${engine_source_hash}")
endforeach()
string(SHA256 HOGD_RV32_ENGINE_SOURCE_DIGEST
  "${HOGD_RV32_ENGINE_DIGEST_INPUT}")
string(SUBSTRING "${HOGD_RV32_ENGINE_SOURCE_DIGEST}" 0 16
  HOGD_RV32_ENGINE_SOURCE_DIGEST)
# Re-run CMake when any of them changes, so the digest cannot go stale.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${HOGD_RV32_ENGINE_SOURCES})

string(TOUPPER "${CMAKE_BUILD_TYPE}" HOGD_RV32_GAS_BUILD_TYPE_UPPER)
set(HOGD_RV32_GAS_CXX_FLAGS
  "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${HOGD_RV32_GAS_BUILD_TYPE_UPPER}}")
# The interpreter's own options follow the global flags (see
# rv32_runtime_objects); record them too, or a frame-pointer-free engine is
# reported as built with -fno-omit-frame-pointer.
if(HOGD_RV32_INTERPRETER_COMPILE_OPTIONS)
  string(APPEND HOGD_RV32_GAS_CXX_FLAGS
    " ${HOGD_RV32_INTERPRETER_COMPILE_OPTIONS}")
endif()
target_compile_definitions(
  rv32_gas_calibration
  PUBLIC
  RV32_GAS_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
  RV32_GAS_ENGINE_SOURCE_DIGEST="${HOGD_RV32_ENGINE_SOURCE_DIGEST}"
  RV32_GAS_CXX_FLAGS="${HOGD_RV32_GAS_CXX_FLAGS}"
)

# Calibrate the interpreter as the node builds it.
target_compile_options(
  rv32_gas_calibration
  PRIVATE
  ${HOGD_RV32_INTERPRETER_COMPILE_OPTIONS}
)

# Match the parent RV32 target's interprocedural optimization setting.
if(HOGD_RV32_ENABLE_LTO)
  set_property(
    TARGET rv32_gas_calibration
    PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE
  )
endif()

add_executable(
  rv32_gas_calibrate
  rv32_gas_calibrate.cc
)

target_link_libraries(
  rv32_gas_calibrate
  rv32_gas_calibration
)

target_compile_options(
  rv32_gas_calibrate
  PRIVATE
  ${HOGD_RV32_INTERPRETER_COMPILE_OPTIONS}
)

if(HOGD_RV32_ENABLE_LTO)
  set_property(
    TARGET rv32_gas_calibrate
    PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE
  )
endif()

# Default tests check encoding, execution accounting, and lowering audits.
# They assert generated-program behavior, not timing thresholds.
add_executable(
  rv32_gas_calibration_test
  rv32_gas_calibration_test.cc
  ../rv32_vm_test_main.cc
)

target_link_libraries(
  rv32_gas_calibration_test
  rv32_gas_calibration
  GTest::gtest
)

hogd_discover_tests(rv32_gas_calibration_test TEST_PREFIX rv32.gas_calibration.)
