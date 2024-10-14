
if(X23_MAKE_ALL_DEPEND_ON_TESTS)
  set(EXCLUDE_TOKEN)
else()
  set(EXCLUDE_TOKEN EXCLUDE_FROM_ALL)
endif()


set(TEST_EXECUTABLES)
macro(add_x23_test TEST_NAME)
  add_executable(test_${TEST_NAME} ${EXCLUDE_TOKEN} ${TEST_NAME}.cc
    xdrtest.x ${ARGN})
  target_link_libraries(test_${TEST_NAME} x23)
  use_x23(test_${TEST_NAME})
  add_test(NAME ${TEST_NAME} COMMAND test_${TEST_NAME})
  list(APPEND TEST_EXECUTABLES test_${TEST_NAME})
endmacro()

add_x23_test(callback)
add_x23_test(cleanup)
add_x23_test(event)
add_x23_test(format)
add_x23_test(marshal)
add_x23_test(rpc)
add_x23_test(uvec)
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

add_x23_gtest(xdr)

if(BENCHMARK_PATH)
add_executable(bench bench.cc xdrtest.x)
use_x23(bench)
target_link_libraries(bench x23 benchmark::benchmark)
endif()

add_custom_target(build_tests DEPENDS ${TEST_EXECUTABLES})
