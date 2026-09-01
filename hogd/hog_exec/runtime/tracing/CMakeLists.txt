add_library(tracing_test_objects OBJECT
  error_trace_tests.cc
  syscall_output_trace_tests.cc
  trace_shape_tests.cc
)

target_link_libraries(
  tracing_test_objects
  GTest::gtest
  hogd_interface
)

target_include_directories(
  tracing_test_objects
  PRIVATE
  ${CMAKE_BINARY_DIR}
)

# trace_shape_tests.cc includes the generated built_contracts/sha256_wrapper.h
# and loads its .rv32a artifact.
add_dependencies(
  tracing_test_objects
  all_contract_headers
)

add_executable(
  tracing_tests
  tracing_test_main.cc
  $<TARGET_OBJECTS:tracing_test_objects>
)

target_link_libraries(
  tracing_tests
  precompiles
  runtime_test_lib
  contract_loader
  storage_cost
  model_checker
  GTest::gtest
)

add_dependencies(
  tracing_tests
  all_riscvs_built # for sha256_wrapper.rv32a
)

gtest_discover_tests(tracing_tests TEST_PREFIX tracing.)
