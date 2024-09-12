
set(TEST_EXECUTABLES)
macro(add_x23_test TEST_NAME)
  add_executable(test_${TEST_NAME} EXCLUDE_FROM_ALL ${TEST_NAME}.cc xdrtest.x)
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

add_custom_target(build_tests DEPENDS ${TEST_EXECUTABLES})
