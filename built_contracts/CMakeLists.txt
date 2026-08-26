# Get the relative path of the current source directory
cmake_path(RELATIVE_PATH CMAKE_CURRENT_SOURCE_DIR BASE_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE CURRENT_RELATIVE_SOURCE_DIR)

# Add contract loader library
add_library(contract_loader
  contract_loader.cc
)

target_link_libraries(contract_loader
  base
  built_headers_interface
  x23
  protocol
  unified_lib
)

target_include_directories(contract_loader PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/hogd/xdr
  ${CMAKE_SOURCE_DIR}
)

target_include_directories(contract_loader PRIVATE
  ${CMAKE_BINARY_DIR}
)

add_custom_target(all_contract_headers)
add_custom_target(all_riscv_contracts)

# Tell CMake to clean the cargo/groundhog intermediate build directory.
set_property(DIRECTORY APPEND PROPERTY ADDITIONAL_CLEAN_FILES
  "${CMAKE_CURRENT_BINARY_DIR}/cargo-target"
)

set(generated_rs_files)

add_xdr_rs(
	cxs_roundtrip_rs
	${CMAKE_CURRENT_SOURCE_DIR}/integration_test_contracts/cxs_roundtrip/src
	./integration_test_contracts/xdr/roundtrip.x)
list(APPEND generated_rs_files ${CMAKE_CURRENT_SOURCE_DIR}/integration_test_contracts/cxs_roundtrip/src/roundtrip.rs)

add_xdr_rs(
	txs_roundtrip_rs
	${CMAKE_CURRENT_SOURCE_DIR}/integration_test_contracts/txs_roundtrip/src
	./integration_test_contracts/xdr/roundtrip.x)
list(APPEND generated_rs_files ${CMAKE_CURRENT_SOURCE_DIR}/integration_test_contracts/txs_roundtrip/src/roundtrip.rs)

add_xdr(roundtrip_hh
  ./integration_test_contracts/xdr/roundtrip.x
)
add_dependencies(all_contract_headers build_roundtrip_hh)

set(roundtrip_rs_stamp ${CMAKE_CURRENT_BINARY_DIR}/roundtrip_rs.stamp)
add_custom_command(
  OUTPUT ${roundtrip_rs_stamp}
  COMMAND ${CMAKE_COMMAND} -E touch ${roundtrip_rs_stamp}
  DEPENDS ${generated_rs_files}
  COMMENT "Roundtrip RS files generated"
)
add_custom_target(build_roundtrip_rs DEPENDS ${roundtrip_rs_stamp})

# Build all RV32 contracts via build_risc.sh (groundhog-cli)
# Usage: add_riscv_builds(SPEC_CONTRACTS d1/c1 d2/c2 ... RAW_CONTRACTS d3/c3 ...)
# Contracts are named by their crate directory relative to this directory
# (e.g. integration_test_contracts/fibonacci, local_test_contracts/foo); the
# cargo package name is the last path component. Outputs land flat in
# ${CMAKE_CURRENT_BINARY_DIR}, so package names must be unique across all
# contract directories.
# SPEC_CONTRACTS produce <name>.rv32a + <name>.rv32.json + <name>_spec.json
# RAW_CONTRACTS produce only <name>.rv32a + <name>.rv32.json (no #[contract]
# macro, no spec)
function(add_riscv_builds)
  cmake_parse_arguments(ARG "" "" "SPEC_CONTRACTS;RAW_CONTRACTS" ${ARGN})

  set(common_depends
    ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.lock
    ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml
    ${roundtrip_rs_stamp})

  set(all_outputs "")
  set(all_depends ${common_depends})
  set(build_commands "")

  foreach(contract_path ${ARG_SPEC_CONTRACTS})
    cmake_path(GET contract_path FILENAME target_name)
    list(APPEND all_outputs ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.rv32a)
    list(APPEND all_outputs ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.rv32.json)
    list(APPEND all_outputs ${CMAKE_CURRENT_BINARY_DIR}/${target_name}_spec.json)

    file(GLOB target_rs_files
      ${CMAKE_CURRENT_SOURCE_DIR}/${contract_path}/src/*.rs)
    list(FILTER target_rs_files EXCLUDE REGEX "roundtrip\\.rs$")

    list(APPEND all_depends
      ${CMAKE_CURRENT_SOURCE_DIR}/${contract_path}/Cargo.toml
      ${target_rs_files})

    list(APPEND build_commands
      COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/build_risc.sh
        ${target_name}
        ${CMAKE_CURRENT_BINARY_DIR})
  endforeach()

  foreach(contract_path ${ARG_RAW_CONTRACTS})
    cmake_path(GET contract_path FILENAME target_name)
    list(APPEND all_outputs ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.rv32a)
    list(APPEND all_outputs ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.rv32.json)

    file(GLOB target_rs_files
      ${CMAKE_CURRENT_SOURCE_DIR}/${contract_path}/src/*.rs)
    list(FILTER target_rs_files EXCLUDE REGEX "roundtrip\\.rs$")

    list(APPEND all_depends
      ${CMAKE_CURRENT_SOURCE_DIR}/${contract_path}/Cargo.toml
      ${target_rs_files})

    list(APPEND build_commands
      COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/build_risc.sh
        --no-spec
        ${target_name}
        ${CMAKE_CURRENT_BINARY_DIR})
  endforeach()

  add_custom_command(
    OUTPUT ${all_outputs}
    ${build_commands}
    DEPENDS ${all_depends}
    COMMENT "Building all RV32 contracts via build_risc.sh"
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  )

  add_custom_target(all_riscvs_built DEPENDS ${all_outputs})
  add_dependencies(all_riscv_contracts all_riscvs_built)
endfunction()

# Generate .x file from spec JSON type_defs
function(add_types_xdr target_name)
  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.x
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/generate_types_xdr.py
        ${CMAKE_CURRENT_BINARY_DIR}/${target_name}_spec.json
        -o ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.x
        -n ${target_name}
    DEPENDS
      ${CMAKE_CURRENT_SOURCE_DIR}/generate_types_xdr.py
      ${CMAKE_CURRENT_BINARY_DIR}/${target_name}_spec.json
    COMMENT "Generating ${target_name}.x from spec type_defs"
  )
endfunction()

# Generate C++ header from spec JSON
function(add_contract_header target_name)

  # Parse any extra includes passed as additional arguments
  set(extra_includes "")
  foreach(arg ${ARGN})
    list(APPEND extra_includes "-i" ${arg})
  endforeach()

  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.h
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/generate_header.py
        ${CMAKE_CURRENT_BINARY_DIR}/${target_name}_spec.json
        -o ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.h
        -n ${target_name}
        -r ${target_name}.rv32a
        ${extra_includes}
    DEPENDS
      ${CMAKE_CURRENT_SOURCE_DIR}/generate_header.py
      ${CMAKE_CURRENT_BINARY_DIR}/${target_name}_spec.json
    COMMENT "Generating ${target_name}.h from spec"
  )

  add_custom_target(${target_name}_header
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.h)
  # Ensure rv32 build (which produces the spec) completes first
  add_dependencies(${target_name}_header all_riscvs_built)

  # make sure that other targets can depend on the generated headers,
  # not just the contracts
  add_dependencies(all_contract_headers ${target_name}_header)
endfunction()

add_executable(contract_loader_test contract_loader_test.cc)
target_link_libraries(contract_loader_test
  contract_loader
  GTest::gtest_main
)
add_dependencies(contract_loader_test all_riscv_contracts all_riscvs_built)
gtest_discover_tests(contract_loader_test)

# Build the contracts for RV32 (each gets a [[bin]] wrapper + .rv32a). The
# SPEC_CONTRACTS also produce <name>_spec.json, consumed by the header/XDR
# generators below.
#
# Contracts live in two sibling directories: integration_test_contracts (a
# submodule) and local_test_contracts (checked into this repo directly).
#
# A few contracts are intentionally excluded because they cannot be lowered to
# an RV32 artifact:
#   - calldata_raw, sha256_raw, fibonacci_raw: no #[contract] macro and no
#     [[bin]] wrapper, so there is no RV32 entry point to lower.
#   - nocontract_wallet: client-stub-only crate, not built for either target.
add_riscv_builds(
  SPEC_CONTRACTS
    integration_test_contracts/fibonacci
    integration_test_contracts/fibonacci_call
    integration_test_contracts/default_wallet
    integration_test_contracts/dev_wallet
    integration_test_contracts/deploy_and_transfer
    integration_test_contracts/genesis_root
    integration_test_contracts/read_only_test
    integration_test_contracts/manual_revert_test
    integration_test_contracts/manual_revert_caller
    integration_test_contracts/simple_wallet
    integration_test_contracts/simple_wallet_authclaim
    integration_test_contracts/sha256_wrapper
    integration_test_contracts/itc_erc20
    integration_test_contracts/authclaim_test
    integration_test_contracts/htlc
    integration_test_contracts/payment_channel
    integration_test_contracts/return_fault_test
    integration_test_contracts/tuple_return
    integration_test_contracts/no_syscall_test
    integration_test_contracts/syscall_misc_tests
    integration_test_contracts/syscall_parse_hardening_test
    integration_test_contracts/nnint_output_fault_test
    integration_test_contracts/output_alias_test
    integration_test_contracts/calldata_wrapper
    integration_test_contracts/delegate_call_test
    integration_test_contracts/ed25519_verify_wrapper
    integration_test_contracts/view_gateway_test
    integration_test_contracts/cxs_roundtrip
    integration_test_contracts/txs_roundtrip
    local_test_contracts/airdrop_faucet
    local_test_contracts/auth_weight_test
    local_test_contracts/drip_faucet
    local_test_contracts/faucet_caller
    local_test_contracts/tx_context_test
  RAW_CONTRACTS
    integration_test_contracts/hash_bug
)

# Generate .x files from spec JSON type_defs for contracts that define custom types
add_types_xdr(default_wallet)
add_types_xdr(drip_faucet)
add_types_xdr(genesis_root)
add_types_xdr(itc_erc20)
add_types_xdr(manual_revert_test)
add_types_xdr(simple_wallet)
add_types_xdr(txs_roundtrip)
add_types_xdr(cxs_roundtrip)
add_types_xdr(payment_channel)

# XDR .x -> .hh via x23c
add_xdr(default_wallet_hh ./default_wallet.x)
add_xdr(drip_faucet_hh ./drip_faucet.x)
add_xdr(genesis_root_hh ./genesis_root.x)
add_xdr(itc_erc20_hh ./itc_erc20.x)
add_xdr(manual_revert_test_hh ./manual_revert_test.x)
add_xdr(simple_wallet_hh ./simple_wallet.x)
add_xdr(txs_roundtrip_hh ./txs_roundtrip.x)
add_xdr(cxs_roundtrip_hh ./cxs_roundtrip.x)
add_xdr(payment_channel_hh ./payment_channel.x)

# Ensure XDR generation waits for spec JSONs (produced by rv32 build)
add_dependencies(build_default_wallet_hh all_riscvs_built)
add_dependencies(build_drip_faucet_hh all_riscvs_built)
add_dependencies(build_genesis_root_hh all_riscvs_built)
add_dependencies(build_itc_erc20_hh all_riscvs_built)
add_dependencies(build_manual_revert_test_hh all_riscvs_built)
add_dependencies(build_simple_wallet_hh all_riscvs_built)
add_dependencies(build_txs_roundtrip_hh all_riscvs_built)
add_dependencies(build_cxs_roundtrip_hh all_riscvs_built)
add_dependencies(build_payment_channel_hh all_riscvs_built)

# Make all headers wait for all type XDR .hh files
add_dependencies(all_contract_headers all_riscvs_built)

# Generate contract headers
add_contract_header(fibonacci)
add_contract_header(fibonacci_call)
add_contract_header(cxs_roundtrip "${CURRENT_RELATIVE_SOURCE_DIR}/cxs_roundtrip.hh")
add_contract_header(txs_roundtrip "${CURRENT_RELATIVE_SOURCE_DIR}/txs_roundtrip.hh")
add_contract_header(default_wallet "${CURRENT_RELATIVE_SOURCE_DIR}/default_wallet.hh")
add_contract_header(dev_wallet)
add_contract_header(deploy_and_transfer)
add_contract_header(genesis_root "${CURRENT_RELATIVE_SOURCE_DIR}/genesis_root.hh")
add_contract_header(read_only_test)
add_contract_header(manual_revert_test "${CURRENT_RELATIVE_SOURCE_DIR}/manual_revert_test.hh")
add_contract_header(manual_revert_caller)
add_contract_header(sha256_wrapper)
add_contract_header(simple_wallet "${CURRENT_RELATIVE_SOURCE_DIR}/simple_wallet.hh")
add_contract_header(simple_wallet_authclaim)
add_contract_header(itc_erc20 "${CURRENT_RELATIVE_SOURCE_DIR}/itc_erc20.hh")
add_contract_header(authclaim_test)
add_contract_header(htlc)
add_contract_header(payment_channel "${CURRENT_RELATIVE_SOURCE_DIR}/payment_channel.hh")
add_contract_header(return_fault_test)
add_contract_header(tuple_return)
add_contract_header(no_syscall_test)
add_contract_header(ed25519_verify_wrapper)
add_contract_header(syscall_misc_tests)
add_contract_header(syscall_parse_hardening_test)
add_contract_header(nnint_output_fault_test)
add_contract_header(output_alias_test)
add_contract_header(calldata_wrapper)
add_contract_header(view_gateway_test)
add_contract_header(airdrop_faucet)
add_contract_header(auth_weight_test)
add_contract_header(drip_faucet "${CURRENT_RELATIVE_SOURCE_DIR}/drip_faucet.hh")
add_contract_header(faucet_caller)
add_contract_header(tx_context_test)

add_dependencies(default_wallet_header default_wallet_hh)
add_dependencies(genesis_root_header genesis_root_hh)
add_dependencies(itc_erc20_header itc_erc20_hh)
add_dependencies(payment_channel_header payment_channel_hh)
add_dependencies(cxs_roundtrip_header cxs_roundtrip_hh)
add_dependencies(txs_roundtrip_header txs_roundtrip_hh)
add_dependencies(manual_revert_test_header manual_revert_test_hh)
add_dependencies(simple_wallet_header simple_wallet_hh)

# Create ONE top-level ALL target that triggers everything
add_custom_target(build_all_contracts ALL)
add_dependencies(build_all_contracts all_contract_headers all_riscv_contracts)
