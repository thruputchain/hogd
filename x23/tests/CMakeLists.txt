
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
add_x23_test(event)
add_x23_test(format)
add_x23_test(heap)
add_x23_test(marshal)
add_x23_test(rpc)
add_x23_test(uvec)
add_x23_test(rpcthroughput)
add_x23_test(srpcthroughput)
add_x23_test(namespace_check xdr_namespace_test1.x xdr_namespace_test2.x xdr_namespace_test3.x)

# flaky tests: prefer timeout failure over stuck CI
set_tests_properties(event PROPERTIES TIMEOUT 10)
set_tests_properties(rpcthroughput PROPERTIES TIMEOUT 10)
#set_tests_properties(rpcthroughput PROPERTIES DISABLED TRUE)

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

add_x23_gtest(shutdown)
add_x23_gtest(xdr)
add_x23_gtest(sync)
add_x23_gtest(uiobuf)
add_x23_gtest(cxs_serialize_test)

set_tests_properties(shutdown PROPERTIES TIMEOUT 10)

if(BENCHMARK_PATH)
add_executable(bench bench.cc)
use_x23(bench)
target_link_libraries(bench x23 benchmark::benchmark xdrtest)
endif()

add_custom_target(build_tests DEPENDS ${TEST_EXECUTABLES})
