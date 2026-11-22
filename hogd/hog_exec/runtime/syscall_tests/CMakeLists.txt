# Tests whose subject is one syscall, or the syscall layer itself: argument
# parsing and aliasing, faults, gas, and the error each syscall reports.
add_library(syscall_test_objects OBJECT
  bytestring_limit_test.cc
  input_alias_test_rv32_test.cc
  math_syscall_test_rv32_test.cc
  nnint_add_test.cc
  nnint_get_test.cc
  nnint_output_fault_test_rv32_test.cc
  output_alias_test_rv32_test.cc
  return_fault_test_rv32_test.cc
  sha256_incremental_test.cc
  storage_flags_test.cc
  syscall_auth_config_test.cc
  syscall_auth_token_test.cc
  syscall_block_data_test.cc
  syscall_buffer_test.cc
  syscall_entry_outer_tests.cc
  syscall_error_handling_tests.cc
  syscall_misc_tests.cc
  syscall_parse_hardening_rv32_test.cc
  syscall_parse_tests.cc
  syscall_ristretto255_msm_test.cc
  syscall_verify_signature_test.cc
)

target_link_libraries(
  syscall_test_objects
  GTest::gtest
  hogd_interface
)

target_include_directories(
  syscall_test_objects
  PRIVATE
  ${CMAKE_BINARY_DIR}
)

# Several tests include the generated built_contracts/<contract>.h headers and
# the generated syscall_inner.h / syscall_meta.h.
add_dependencies(
  syscall_test_objects
  all_contract_headers
  generate_syscall_inner_h
  generate_syscall_meta_h
)

add_executable(
  syscall_test
  syscall_test_main.cc
  $<TARGET_OBJECTS:syscall_test_objects>
)

target_link_libraries(
  syscall_test
  # Test-only: makes the ML-DSA signatures syscall_verify_signature_test checks.
  mldsa_signer
  precompiles
  runtime
  runtime_test_lib
  runtime_test_util
  hog_db
  storage_cost
  model_checker
  contract_loader
  genesis
  GTest::gtest
)

# The *_rv32_test.cc tests load their contracts' .rv32a artifacts.
add_dependencies(
  syscall_test
  all_riscvs_built
)

hogd_discover_tests(syscall_test)
