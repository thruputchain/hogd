
if(X23_MAKE_ALL_DEPEND_ON_TESTS)
  set(EXCLUDE_TOKEN)
else()
  set(EXCLUDE_TOKEN EXCLUDE_FROM_ALL)
endif()

add_xdr(xdrtest xdrtest.x)

set(TEST_EXECUTABLES)
macro(add_x23_test TEST_NAME)
  add_executable(test_${TEST_NAME} ${EXCLUDE_TOKEN} ${TEST_NAME}.cc
    ${ARGN})
  target_link_libraries(test_${TEST_NAME} x23 xdrtest)
  use_x23(test_${TEST_NAME})
  add_test(NAME ${TEST_NAME} COMMAND test_${TEST_NAME})
  list(APPEND TEST_EXECUTABLES test_${TEST_NAME})
endmacro()

add_x23_test(affinity)
add_x23_test(callback)
add_x23_test(cleanup)
add_x23_test(corosleep)
add_x23_test(corowake)
add_x23_test(event)
add_x23_test(format)
add_x23_test(heap)
add_x23_test(marshal)
add_x23_test(rpc)
add_x23_test(uvec)
add_executable(test_rpcstress ${EXCLUDE_TOKEN} rpcstress.cc)
target_link_libraries(test_rpcstress x23 xdrtest)
use_x23(test_rpcstress)
list(APPEND TEST_EXECUTABLES test_rpcstress)
foreach(scenario bare fanout teardown writebuf shutdown)
  add_test(NAME rpcstress_${scenario}
    COMMAND test_rpcstress -s ${scenario} -d 5 -t 8)
  set_tests_properties(rpcstress_${scenario} PROPERTIES
    TIMEOUT 60 RESOURCE_LOCK rpcstress)
endforeach()

add_x23_test(poll_zombie)
set_tests_properties(poll_zombie PROPERTIES TIMEOUT 10)

add_x23_test(poll_writer_zombie)
set_tests_properties(poll_writer_zombie PROPERTIES TIMEOUT 10)

add_x23_test(writebuf_unlock_uaf)
# The bug this catches is a write into a freed WriteBuffer, so make the
# allocator fill freed memory: in a non-sanitizer debug build that is what
# turns the stray unlock into an x23assert failure.  Ignored by asan,
# which reports the use-after-free directly.
set_tests_properties(writebuf_unlock_uaf PROPERTIES TIMEOUT 30
  ENVIRONMENT "MALLOC_PERTURB_=85;MallocScribble=1")

add_x23_test(abort_zombie)
set_tests_properties(abort_zombie PROPERTIES TIMEOUT 10)

add_x23_test(abort_live)
set_tests_properties(abort_live PROPERTIES TIMEOUT 10)

add_executable(test_canary ${EXCLUDE_TOKEN} canary.cc)
target_link_libraries(test_canary x23)
list(APPEND TEST_EXECUTABLES test_canary)
# Not registered as a ctest -- long-running soak test for coroutine frame
# integrity.  Deterministic coverage is in the detached gtest instead.

add_executable(test_rpc_dispatch_race ${EXCLUDE_TOKEN} rpc_dispatch_race.cc)
target_link_libraries(test_rpc_dispatch_race x23 xdrtest)
use_x23(test_rpc_dispatch_race)
list(APPEND TEST_EXECUTABLES test_rpc_dispatch_race)
add_test(NAME rpc_dispatch_race COMMAND test_rpc_dispatch_race -d 5 -t 8)
set_tests_properties(rpc_dispatch_race PROPERTIES TIMEOUT 60)

add_executable(test_shutdown_race ${EXCLUDE_TOKEN} shutdown_race.cc)
target_link_libraries(test_shutdown_race x23 xdrtest)
use_x23(test_shutdown_race)
list(APPEND TEST_EXECUTABLES test_shutdown_race)
# Not registered as a ctest -- this test deliberately uses an aggressive
# shutdown path (no poison, no fail) that triggers a known destruction
# ordering race.  Build-only; run manually with MALLOC_PERTURB_=85.

add_x23_test(rpcthroughput)
add_x23_test(srpcthroughput latency.h)
add_x23_test(namespace_check xdr_namespace_test1.x xdr_namespace_test2.x xdr_namespace_test3.x)

if(GOOGLETEST_PATH)
include_directories(${GOOGLETEST_PATH}/googletest/include)
set(GTEST_ALL ${GOOGLETEST_PATH}/googletest/src/gtest-all.cc)
set_source_files_properties(${GTEST_ALL} PROPERTIES
  INCLUDE_DIRECTORIES "${GOOGLETEST_PATH}/googletest")
macro(add_x23_gtest TEST_NAME)
  add_x23_test(${TEST_NAME}
    ${GTEST_ALL} ${GOOGLETEST_PATH}/googletest/src/gtest_main.cc)
endmacro()
else()
macro(add_x23_gtest TEST_NAME)
endmacro()
endif()

add_x23_gtest(str_cstr)
add_x23_gtest(cxs_serialize_test)
add_x23_gtest(deserialize)
add_x23_gtest(multiclient)
add_x23_gtest(nmvec)
add_x23_gtest(max_message_size)
add_x23_gtest(srpc_sockio_regression)
add_x23_gtest(shutdown)
add_x23_gtest(sync)
add_x23_gtest(threadpool)
add_x23_gtest(detached)
add_x23_gtest(dag)
add_x23_gtest(uiobuf)
add_x23_gtest(xdr)
add_x23_gtest(refcnt)

if(GOOGLETEST_PATH)
set_tests_properties(shutdown threadpool PROPERTIES TIMEOUT 10)
endif()

if(BENCHMARK_PATH)
add_executable(bench bench.cc)
use_x23(bench)
target_link_libraries(bench x23 benchmark::benchmark xdrtest)
endif()

option(BUILD_FUZZERS "Build fuzz targets" OFF)
if(BUILD_FUZZERS)
  add_executable(fuzz_deserialize EXCLUDE_FROM_ALL fuzz_deserialize.cc)
  target_link_libraries(fuzz_deserialize x23 xdrtest)
  use_x23(fuzz_deserialize)
  target_compile_options(fuzz_deserialize PRIVATE -fsanitize=fuzzer,address)
  # libclang_rt.fuzzer is built against libstdc++, but this project uses
  # libc++. Suppress automatic stdlib linking and explicitly link both.
  target_link_options(fuzz_deserialize PRIVATE -fsanitize=fuzzer,address -nostdlib++ -lc++ -lstdc++)
endif()

add_custom_target(build_tests DEPENDS ${TEST_EXECUTABLES})
