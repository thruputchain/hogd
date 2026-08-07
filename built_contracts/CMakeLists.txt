# Get the relative path of the current source directory
cmake_path(RELATIVE_PATH CMAKE_CURRENT_SOURCE_DIR BASE_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE CURRENT_RELATIVE_SOURCE_DIR)

# Add contract loader library
add_library(contract_loader
  contract_loader.cc
  ${CMAKE_SOURCE_DIR}/hogd/hog_exec/runtime/wasm/instrumented_bytes.cc
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

add_dependencies(contract_loader generate_wasm_meter_ffi_h)

add_custom_target(all_wasm_contracts)
add_custom_target(all_wasm_headers)
add_dependencies(all_wasm_contracts all_wasm_headers)

add_custom_target(all_riscv_contracts)

# Configure WASM build flags
if(WASM_DEBUG_BACKTRACES)
  set(WASM_FLAGS "-F logging,entrypoints")
  message(STATUS "WASM contracts: debug build with backtraces enabled")
else()
  set(WASM_FLAGS "-F entrypoints")
  message(STATUS "WASM contracts: release build (minimal size, no backtraces)")
endif()

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
add_dependencies(all_wasm_headers build_roundtrip_hh)

set(roundtrip_rs_stamp ${CMAKE_CURRENT_BINARY_DIR}/roundtrip_rs.stamp)
add_custom_command(
  OUTPUT ${roundtrip_rs_stamp}
  COMMAND ${CMAKE_COMMAND} -E touch ${roundtrip_rs_stamp}
  DEPENDS ${generated_rs_files}
  COMMENT "Roundtrip RS files generated"
)
add_custom_target(build_roundtrip_rs DEPENDS ${roundtrip_rs_stamp})

# Build all WASM contracts via groundhog-cli
# Usage: add_wasm_builds(SPEC_CONTRACTS c1 c2 ... RAW_CONTRACTS c3 c4 ...)
# SPEC_CONTRACTS produce both .wasm and _spec.json
# RAW_CONTRACTS produce only .wasm (no #[contract] macro, no embedded spec)
function(add_wasm_builds)
  cmake_parse_arguments(ARG "" "" "SPEC_CONTRACTS;RAW_CONTRACTS" ${ARGN})

  # Common dependencies (use stamp file to ensure roundtrip.rs files exist)
  set(common_depends
    ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.lock
    ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml
    ${roundtrip_rs_stamp})

  # If you update the contracts (i.e. add new contracts, or change dependencies)
  # then Cargo.lock will change, and the build will fail.  If that happens,
  # run cargo update to update the lockfile manually.
  #
  # This will not fetch crates during the build process; you need to run the fetch script
  # `./scripts/fetch.sh`
  # if your set of downloaded crates is incomplete.

  set(all_outputs "")
  set(all_depends ${common_depends})
  set(build_commands "")

  foreach(target_name ${ARG_SPEC_CONTRACTS})
    list(APPEND all_outputs ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.wasm)
    list(APPEND all_outputs ${CMAKE_CURRENT_BINARY_DIR}/${target_name}_spec.json)

    file(GLOB target_rs_files
      ${CMAKE_CURRENT_SOURCE_DIR}/integration_test_contracts/${target_name}/src/*.rs)
    list(FILTER target_rs_files EXCLUDE REGEX "roundtrip\\.rs$")

    list(APPEND all_depends
      ${CMAKE_CURRENT_SOURCE_DIR}/integration_test_contracts/${target_name}/Cargo.toml
      ${target_rs_files})

    list(APPEND build_commands
      COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/build_wasm.sh
        ${target_name}
        ${CMAKE_CURRENT_BINARY_DIR}
        "${WASM_FLAGS}")
  endforeach()

  foreach(target_name ${ARG_RAW_CONTRACTS})
    list(APPEND all_outputs ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.wasm)

    file(GLOB target_rs_files
      ${CMAKE_CURRENT_SOURCE_DIR}/integration_test_contracts/${target_name}/src/*.rs)
    list(FILTER target_rs_files EXCLUDE REGEX "roundtrip\\.rs$")

    list(APPEND all_depends
      ${CMAKE_CURRENT_SOURCE_DIR}/integration_test_contracts/${target_name}/Cargo.toml
      ${target_rs_files})

    list(APPEND build_commands
      COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/build_wasm.sh
        --no-spec
        ${target_name}
        ${CMAKE_CURRENT_BINARY_DIR}
        "${WASM_FLAGS}")
  endforeach()

  add_custom_command(
    OUTPUT ${all_outputs}
    ${build_commands}
    DEPENDS ${all_depends}
    COMMENT "Building all WASM contracts via groundhog-cli"
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  )

  add_custom_target(all_wasms_built DEPENDS ${all_outputs})
  add_dependencies(all_wasm_contracts all_wasms_built)
endfunction()

# Build all RV32 contracts via build_risc.sh
# Usage: add_riscv_builds(CONTRACTS c1 c2 ...)
# Each contract's [[bin]] target is compiled to an RV32 ELF and lowered to
# <name>.rv32a (+ <name>.rv32.json), alongside the WASM artifacts.
function(add_riscv_builds)
  cmake_parse_arguments(ARG "" "" "CONTRACTS" ${ARGN})

  set(common_depends
    ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.lock
    ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml
    ${roundtrip_rs_stamp})

  set(all_outputs "")
  set(all_depends ${common_depends})
  set(build_commands "")

  foreach(target_name ${ARG_CONTRACTS})
    list(APPEND all_outputs ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.rv32a)
    list(APPEND all_outputs ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.rv32.json)

    file(GLOB target_rs_files
      ${CMAKE_CURRENT_SOURCE_DIR}/integration_test_contracts/${target_name}/src/*.rs)
    list(FILTER target_rs_files EXCLUDE REGEX "roundtrip\\.rs$")

    list(APPEND all_depends
      ${CMAKE_CURRENT_SOURCE_DIR}/integration_test_contracts/${target_name}/Cargo.toml
      ${target_rs_files})

    list(APPEND build_commands
      COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/build_risc.sh
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
        -w ${target_name}.wasm
        ${extra_includes}
    DEPENDS
      ${CMAKE_CURRENT_SOURCE_DIR}/generate_header.py
      ${CMAKE_CURRENT_BINARY_DIR}/${target_name}_spec.json
    COMMENT "Generating ${target_name}.h from spec"
  )

  add_custom_target(${target_name}_header
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.h)
  # Ensure wasm build (which produces the spec) completes first
  add_dependencies(${target_name}_header all_wasms_built)

  # make sure that other targets can depend on the generated headers,
  # not just the wasms
  add_dependencies(all_wasm_headers ${target_name}_header)
endfunction()

# Build all WASMs (and specs) via groundhog-cli
add_wasm_builds(
  SPEC_CONTRACTS
    fibonacci
    fibonacci_call
    cxs_roundtrip
    txs_roundtrip
    default_wallet
    dev_wallet
    deploy_and_transfer
    read_only_test
    manual_revert_test
    manual_revert_caller
    simple_wallet
    simple_wallet_authclaim
    sha256_wrapper
    initial_token_claim
    itc_erc20
    authclaim_test
    htlc
    payment_channel
    return_fault_test
    tuple_return
    no_syscall_test
    syscall_misc_tests
    syscall_parse_hardening_test
    nnint_output_fault_test
    output_alias_test
    calldata_wrapper
    delegate_call_test
    ed25519_verify_wrapper
    view_gateway_test
  RAW_CONTRACTS
    hash_bug
    calldata_raw
    sha256_raw
    fibonacci_raw
    nocontract_wallet
)

add_executable(contract_loader_test contract_loader_test.cc)
target_link_libraries(contract_loader_test
  contract_loader
  GTest::gtest_main
)
add_dependencies(contract_loader_test all_riscv_contracts all_wasms_built)
gtest_discover_tests(contract_loader_test)

# Build the spec contracts for RV32 (each gets a [[bin]] wrapper + .rv32a).
#
# A few contracts are intentionally excluded because they cannot be lowered to
# an RV32 artifact:
#   - RAW_CONTRACTS (hash_bug, calldata_raw, sha256_raw, fibonacci_raw,
#     nocontract_wallet): no #[contract] macro -> no gh_* entry symbols, so
#     rv32-lower rejects the ELF ("no #[contract] entry-point symbols found").
#     hash_bug additionally has no sdk dep, so the rv32 runtime never links.
#   - cxs_roundtrip, txs_roundtrip: their image overflows the rv32 RAM region
#     (rust-lld: "section will not fit in region 'RAM'").
message(STATUS "RV32 contracts: building 27 spec contracts (excluding raw contracts, cxs_roundtrip, txs_roundtrip)")
add_riscv_builds(
  CONTRACTS
    fibonacci
    fibonacci_call
    default_wallet
    dev_wallet
    deploy_and_transfer
    read_only_test
    manual_revert_test
    manual_revert_caller
    simple_wallet
    simple_wallet_authclaim
    sha256_wrapper
    initial_token_claim
    itc_erc20
    authclaim_test
    htlc
    payment_channel
    return_fault_test
    tuple_return
    no_syscall_test
    syscall_misc_tests
    syscall_parse_hardening_test
    nnint_output_fault_test
    output_alias_test
    calldata_wrapper
    delegate_call_test
    ed25519_verify_wrapper
    view_gateway_test
)

# Generate .x files from spec JSON type_defs for contracts that define custom types
add_types_xdr(default_wallet)
add_types_xdr(itc_erc20)
add_types_xdr(manual_revert_test)
add_types_xdr(simple_wallet)
add_types_xdr(txs_roundtrip)
add_types_xdr(cxs_roundtrip)
add_types_xdr(payment_channel)

# XDR .x -> .hh via x23c
add_xdr(default_wallet_hh ./default_wallet.x)
add_xdr(itc_erc20_hh ./itc_erc20.x)
add_xdr(manual_revert_test_hh ./manual_revert_test.x)
add_xdr(simple_wallet_hh ./simple_wallet.x)
add_xdr(txs_roundtrip_hh ./txs_roundtrip.x)
add_xdr(cxs_roundtrip_hh ./cxs_roundtrip.x)
add_xdr(payment_channel_hh ./payment_channel.x)

# Ensure XDR generation waits for spec JSONs (produced by wasm build)
add_dependencies(build_default_wallet_hh all_wasms_built)
add_dependencies(build_itc_erc20_hh all_wasms_built)
add_dependencies(build_manual_revert_test_hh all_wasms_built)
add_dependencies(build_simple_wallet_hh all_wasms_built)
add_dependencies(build_txs_roundtrip_hh all_wasms_built)
add_dependencies(build_cxs_roundtrip_hh all_wasms_built)
add_dependencies(build_payment_channel_hh all_wasms_built)

# Make all headers wait for all type XDR .hh files
add_dependencies(all_wasm_headers all_wasms_built)

# Generate contract headers
add_contract_header(fibonacci)
add_contract_header(fibonacci_call)
add_contract_header(cxs_roundtrip "${CURRENT_RELATIVE_SOURCE_DIR}/cxs_roundtrip.hh")
add_contract_header(txs_roundtrip "${CURRENT_RELATIVE_SOURCE_DIR}/txs_roundtrip.hh")
add_contract_header(default_wallet "${CURRENT_RELATIVE_SOURCE_DIR}/default_wallet.hh")
add_contract_header(dev_wallet)
add_contract_header(deploy_and_transfer)
add_contract_header(read_only_test)
add_contract_header(manual_revert_test "${CURRENT_RELATIVE_SOURCE_DIR}/manual_revert_test.hh")
add_contract_header(manual_revert_caller)
add_contract_header(sha256_wrapper)
add_contract_header(simple_wallet "${CURRENT_RELATIVE_SOURCE_DIR}/simple_wallet.hh")
add_contract_header(simple_wallet_authclaim)
add_contract_header(initial_token_claim)
add_contract_header(itc_erc20 "${CURRENT_RELATIVE_SOURCE_DIR}/itc_erc20.hh")
add_contract_header(authclaim_test)
add_contract_header(htlc)
add_contract_header(payment_channel "${CURRENT_RELATIVE_SOURCE_DIR}/payment_channel.hh")
add_contract_header(return_fault_test)
add_contract_header(tuple_return)
add_contract_header(no_syscall_test)
add_contract_header(syscall_misc_tests)
add_contract_header(syscall_parse_hardening_test)
add_contract_header(nnint_output_fault_test)
add_contract_header(output_alias_test)
add_contract_header(calldata_wrapper)
add_contract_header(view_gateway_test)

add_dependencies(default_wallet_header default_wallet_hh)
add_dependencies(itc_erc20_header itc_erc20_hh)
add_dependencies(payment_channel_header payment_channel_hh)
add_dependencies(cxs_roundtrip_header cxs_roundtrip_hh)
add_dependencies(txs_roundtrip_header txs_roundtrip_hh)
add_dependencies(manual_revert_test_header manual_revert_test_hh)
add_dependencies(simple_wallet_header simple_wallet_hh)

# Create ONE top-level ALL target that triggers everything
add_custom_target(build_all_contracts ALL)
add_dependencies(build_all_contracts all_wasm_contracts all_riscv_contracts)
