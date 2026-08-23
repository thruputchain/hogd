
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

add_x23_test(abort_zombie)
set_tests_properties(abort_zombie PROPERTIES TIMEOUT 10)

add_x23_test(abort_live)
set_tests_properties(abort_live PROPERTIES TIMEOUT 10)

add_x23_test(writebuf_fd_poison)
set_tests_properties(writebuf_fd_poison PROPERTIES TIMEOUT 15)

add_x23_test(writebuf_stale_worker)
set_tests_properties(writebuf_stale_worker PROPERTIES TIMEOUT 15)

add_x23_test(poll_writer_uaf)
set_tests_properties(poll_writer_uaf PROPERTIES TIMEOUT 15)

# Reproducers for a clang 22 coroutine codegen regression (see the header
# comments): the .destroy clone of a MaybeAsync-returning coroutine destroys
# an uninitialized copy of the ramp's return-object temporary.  Expected to
# fail (SIGSEGV) on toolchains carrying the bug, e.g. Homebrew clang 22.1.5;
# passes on clang 21.  The _standalone variant uses only standard headers so
# it can be handed to LLVM verbatim.
add_x23_test(coro_return_object_destroy)
set_tests_properties(coro_return_object_destroy PROPERTIES TIMEOUT 15)
# add_x23_test(coro_return_object_destroy_standalone)
# set_tests_properties(coro_return_object_destroy_standalone PROPERTIES TIMEOUT 15)

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
set_tests_properties(shutdown PROPERTIES TIMEOUT 10)
# threadpool gets the same budget as the stress tests above rather than
# shutdown's 10s.  DestructionWakeupStress creates and destroys 10000
# thread pools, and its cost tracks the host's CPU count (a pool
# allocates one run queue per LLC bucket and always starts a qcbq
# service thread), so on a big CI host under `ctest --parallel` it takes
# a few seconds even without sanitizers -- 4.5s on the asyncio schedule
# -- and runs past 10s under ASAN or TSAN.  It is checking for a lost
# wakeup, which hangs rather than merely running slowly, so the larger
# budget still catches what the test is for.
set_tests_properties(threadpool PROPERTIES TIMEOUT 60)
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
