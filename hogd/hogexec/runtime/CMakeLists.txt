set_source_files_properties(
    ${CMAKE_CURRENT_BINARY_DIR}/syscall_inner.h
    PROPERTIES GENERATED TRUE)

set_source_files_properties(
    ${CMAKE_CURRENT_BINARY_DIR}/syscall_meta.h
    PROPERTIES GENERATED TRUE)

add_subdirectory(test_util)
add_subdirectory(view)
add_subdirectory(wasm)
add_subdirectory(item)
add_subdirectory(anomalies)
add_subdirectory(direct_rpc_tests)
add_subdirectory(rv32)

add_library(runtime_objects OBJECT
  event_server_proxy.cc
  invocation_frame.cc
  native_runtime.cc
  metrics.cc
  multiplexed_context.cc
  runtime.cc
  syscall_auth.cc
  syscall_block_data.cc
  syscall_bytestring.cc
  syscall_contract.cc
  syscall_crypto.cc
  syscall_delete.cc
  syscall_entry.cc
  syscall_handler.cc
  syscall_invoke.cc
  syscall_log.cc
  syscall_nnint.cc
  syscall_rcq.cc
  syscall_stats.cc
  transaction_stats.cc
)

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  # Arbitrary early-warning threshold for Release host-frame growth.
  # 1536 is not a safety boundary; it is meant to catch runtime/syscall frames
  # before they get out of hand. We started lower, but 1024 produced repeated
  # libc++ std::format noise under clang/LLVM 21.
  target_compile_options(runtime_objects PRIVATE
    $<$<CONFIG:Release>:-Wframe-larger-than=1536>)
endif()

target_link_libraries(
  runtime_objects
  hogd_interface
)

add_library(runtime STATIC
  $<TARGET_OBJECTS:runtime_objects>
  $<TARGET_OBJECTS:wasm_runtime_objects>
  $<TARGET_OBJECTS:rv32_runtime_objects>
)

add_library(runtime_test_lib
  native_contract_test_framework.cc
)

target_link_libraries(
  runtime_test_lib
  runtime
  runtime_test_util # TODO(geoff): remove this if we replace with proper event server connection
  db_interface
  hogdb
  GTest::gtest
)

add_library(runtime_test_objects OBJECT
  authclaim_test.cc
  balance_cost_tracking_test.cc
  block_cancel_tests.cc
  contract_result_preset_handling_test.cc
  contract_trait_test.cc
  cross_call_tests.cc
  cross_contract_revert_test.cc
  delete_recreate_tests.cc
  deserialize_test.cc
  empty_test.cc
  event_test.cc
  fibonacci_test.cc
  sha256_incremental_test.cc
  interleaving_tests.cc
  invocation_frame_test.cc
  invoke_error_handling_test.cc
  invoke_tx_leak_test.cc
  manual_revert_comprehensive_test.cc
  manual_revert_wasm_test.cc
  manual_revert_caller_wasm_test.cc
  return_fault_test_wasm_test.cc
  nnint_output_fault_test_wasm_test.cc
  output_alias_test_wasm_test.cc
  native_contract_test.cc
  native_token_allowance_test.cc
  nnint_add_test.cc
  propose_validate_roundtrip_test.cc
  nnint_get_test.cc
  rcq_delete_test.cc
  rcq_doctored_proposal_data_test.cc
  rcq_interleaving_tests.cc
  rcq_peek_test.cc
  rcq_read_test.cc
  rcq_test.cc
  read_only_test.cc
  replay_cache_integration_test.cc
  revert_tests.cc
  runtime_test.cc
  selective_deployment_test.cc
  serialize_roundtrip_test.cc
  sha2_in_contract_test.cc
  syscall_auth_config_test.cc
  syscall_auth_token_test.cc
  syscall_block_data_test.cc
  syscall_buffer_test.cc
  syscall_entry_outer_tests.cc
  syscall_error_handling_tests.cc
  syscall_misc_tests.cc
  syscall_parse_hardening_wasm_test.cc
  syscall_parse_tests.cc
  tracing_tests.cc
  try_invoke_test.cc
)

target_link_libraries(
  runtime_test_objects
  GTest::gtest
  hogd_interface
)

add_executable(
  runtime_test
  $<TARGET_OBJECTS:runtime_test_objects>
)

target_link_libraries(
  runtime
  hogexec_common
  item
  components
  unified_lib
  protocol
  metrics
  event
  rv32_vm
  hogd
)

add_dependencies(
  runtime_objects
  generate_syscall_inner_h
  generate_syscall_meta_h
  protocol
)

target_include_directories(
  runtime_objects
  PRIVATE
  ${CMAKE_BINARY_DIR}
)

target_include_directories(
  runtime_test_objects
  PRIVATE
  ${CMAKE_BINARY_DIR}
)

target_link_libraries(
  runtime_test
  precompiles
  runtime
  runtime_test_lib
  runtime_test_util
  db_interface
  hogdb
  storage_cost
  model_checker
  contract_loader
  genesis
  GTest::gtest
)
gtest_discover_tests(runtime_test)

add_executable(
  rv32_syscall_entry_bridge_test
  rv32/rv32_syscall_entry_bridge_test.cc
  rv32/rv32_syscall_entry_bridge_test_main.cc
)

target_link_libraries(
  rv32_syscall_entry_bridge_test
  precompiles
  rv32_syscall_bridge
  runtime
  runtime_test_lib
  runtime_test_util
  db_interface
  hogdb
  storage_cost
  model_checker
  contract_loader
  genesis
  GTest::gtest
)
gtest_discover_tests(rv32_syscall_entry_bridge_test
                     TEST_PREFIX rv32.entry.)

add_executable(
  rv32_runtime_context_test
  rv32/runtime_context.cc
  rv32/rv32_runtime_context_test.cc
  rv32/rv32_syscall_entry_bridge_test_main.cc
)

set(HOGD_RV32_LOWER_BIN "" CACHE FILEPATH
    "Optional rv32-lower binary used to build the RV32 SDK smoke artifact")
set(HOGD_RV32_GENERATED_SMOKE_ARTIFACT
    "${CMAKE_CURRENT_BINARY_DIR}/rv32-itc-smoke/rv32_smoke_counter.rv32a")
set(HOGD_RV32_GENERATED_SMOKE_METADATA
    "${CMAKE_CURRENT_BINARY_DIR}/rv32-itc-smoke/rv32_smoke_counter.rv32.json")
if(HOGD_RV32_LOWER_BIN)
  set(HOGD_RV32_SMOKE_ENV
      ${CMAKE_COMMAND} -E env "RV32_LOWER_BIN=${HOGD_RV32_LOWER_BIN}")
else()
  set(HOGD_RV32_SMOKE_ENV)
endif()

add_custom_command(
  OUTPUT
    ${HOGD_RV32_GENERATED_SMOKE_ARTIFACT}
    ${HOGD_RV32_GENERATED_SMOKE_METADATA}
  COMMAND
    ${HOGD_RV32_SMOKE_ENV}
    /bin/sh
    ${CMAKE_SOURCE_DIR}/built_contracts/build_risc.sh
    rv32_smoke_counter
    "${CMAKE_CURRENT_BINARY_DIR}/rv32-itc-smoke"
  DEPENDS
    ${CMAKE_SOURCE_DIR}/built_contracts/build_risc.sh
    ${CMAKE_SOURCE_DIR}/scripts/rv32_make_smoke_artifact.sh
    ${CMAKE_SOURCE_DIR}/built_contracts/Cargo.lock
    ${CMAKE_SOURCE_DIR}/built_contracts/Cargo.toml
    ${CMAKE_SOURCE_DIR}/built_contracts/integration_test_contracts/rv32_smoke_counter/Cargo.toml
    ${CMAKE_SOURCE_DIR}/built_contracts/integration_test_contracts/rv32_smoke_counter/src/lib.rs
    ${CMAKE_SOURCE_DIR}/built_contracts/integration_test_contracts/rv32_smoke_counter/src/main.rs
  COMMENT "Building RV32 ITC smoke artifact"
  VERBATIM
  USES_TERMINAL
)
add_custom_target(
  rv32_itc_smoke_artifact
  DEPENDS ${HOGD_RV32_GENERATED_SMOKE_ARTIFACT}
          ${HOGD_RV32_GENERATED_SMOKE_METADATA}
)
add_dependencies(rv32_runtime_context_test rv32_itc_smoke_artifact)
target_compile_definitions(
  rv32_runtime_context_test
  PRIVATE
  HOGD_RV32_GENERATED_SMOKE_ARTIFACT="${HOGD_RV32_GENERATED_SMOKE_ARTIFACT}"
  HOGD_RV32_GENERATED_SMOKE_METADATA="${HOGD_RV32_GENERATED_SMOKE_METADATA}"
)

target_link_libraries(
  rv32_runtime_context_test
  precompiles
  rv32_syscall_bridge
  runtime
  runtime_test_lib
  runtime_test_util
  db_interface
  hogdb
  storage_cost
  model_checker
  contract_loader
  genesis
  GTest::gtest
)
gtest_discover_tests(rv32_runtime_context_test
                     TEST_PREFIX rv32.runtime.)

add_dependencies(
  rv32_runtime_context_test
  test_error_handling_wasm
  all_wasm_contracts
  all_wasm_headers
)

add_executable(
  rv32_upstream_artifact_test
  rv32/runtime_context.cc
  rv32/rv32_upstream_artifact_test.cc
  rv32/rv32_syscall_entry_bridge_test_main.cc
)

target_link_libraries(
  rv32_upstream_artifact_test
  precompiles
  rv32_syscall_bridge
  runtime
  runtime_test_lib
  runtime_test_util
  db_interface
  hogdb
  storage_cost
  model_checker
  contract_loader
  genesis
  GTest::gtest
)
gtest_discover_tests(
  rv32_upstream_artifact_test
  TEST_PREFIX rv32.upstream.
  NO_PRETTY_VALUES
)

add_dependencies(
  runtime_test_objects
  all_wasm_headers
  build_tests # for x23/tests/xdrtest.hh
)

add_dependencies(
  runtime_test
  all_wasm_contracts
  test_unknown_import_wasm
)
