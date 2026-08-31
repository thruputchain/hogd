# RV32 gas calibration harness.
#
# Core-only, like rv32_isa_core_test and the oracle/threaded tests above it:
# these targets link rv32_isa_core and nothing else -- no OpenSSL, no artifact
# layer, no hogd runtime symbols. That is deliberate and load-bearing. It keeps
# the harness buildable by scripts/rv32_gas_calibrate.sh with a direct compiler
# invocation, so a calibration can be run on a candidate validator machine in
# seconds without configuring the node build.

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

# Stamp the build identity into the binary so a calibration says what it was
# built with. This is not decoration: the numbers are a property of the
# compiled interpreter, and the Release build differs from a casual -O2 one in
# ways that reach the hot path -- -O3, -march, libc++ under clang, LTO. A JSON
# that does not record the flags cannot be compared with one built differently,
# and rv32_gas_calibrate refuses to measure a non-Release build outright.
# Digest of the interpreter's own source. The behavioural fingerprint probes
# which instruction pairs the lowering fuses, which catches a fusion rule, a gas
# cost, or a run limit changing -- and misses a change to a handler body, the
# dispatch path, or the slot layout, all of which move the numbers while leaving
# the fusion matrix identical. That is a false negative in the direction that
# matters: an old calibration would keep looking current.
#
# Hashing the sources catches those. It is deliberately blunt -- a comment-only
# edit moves it too -- because the cost of a spurious "re-run the calibration"
# is a few minutes and the cost of a missed one is a schedule calibrated against
# an interpreter that no longer exists.
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
target_compile_definitions(
  rv32_gas_calibration
  PUBLIC
  RV32_GAS_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
  RV32_GAS_ENGINE_SOURCE_DIGEST="${HOGD_RV32_ENGINE_SOURCE_DIGEST}"
  RV32_GAS_CXX_FLAGS="${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${HOGD_RV32_GAS_BUILD_TYPE_UPPER}}"
)

# The harness measures the production engine, so it has to be built the way
# production is: optimized, with LTO across the interpreter's translation
# units. A non-LTO or unoptimized build would price a different interpreter
# than the node runs. HOGD_RV32_ENABLE_LTO is set by the parent rv32
# CMakeLists for every optimized build type.
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

if(HOGD_RV32_ENABLE_LTO)
  set_property(
    TARGET rv32_gas_calibrate
    PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE
  )
endif()

# Correctness net for the harness itself: kernel encoding against the
# production decoder, exact executed-work accounting, and the lowering audit
# (including that the audit can fail, so a clean audit means something). These
# are ordinary fast unit tests -- they assert properties of generated programs,
# never timings -- so they belong in the default test run.
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

gtest_discover_tests(rv32_gas_calibration_test TEST_PREFIX rv32.gas_calibration.)
