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

add_executable(contract_loader_test contract_loader_test.cc)
target_link_libraries(contract_loader_test
  contract_loader
  GTest::gtest_main
)
add_dependencies(contract_loader_test all_riscv_contracts)
hogd_discover_tests(contract_loader_test)

# Tell CMake to clean the cargo/thruput intermediate build directory.
set_property(DIRECTORY APPEND PROPERTY ADDITIONAL_CLEAN_FILES
  "${CMAKE_CURRENT_BINARY_DIR}/cargo-target"
)

# The roundtrip contracts compile Rust generated from roundtrip.x, which tests
# also use as roundtrip.hh.
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

# add_contract_group(<group> <contract>... [NO_SPEC <contract>...])
#
# Builds the contracts for RV32 with one run of build_risc.sh (thruput-cli)
# per contract, all in a single build step. A contract is named by its crate
# directory relative to this one (e.g. integration_test_contracts/fibonacci);
# its cargo package name is the last path component. Outputs land flat in
# ${CMAKE_CURRENT_BINARY_DIR}, so package names must be unique.
#
# Every contract produces <name>.rv32a and <name>.rv32.json. Every contract not
# listed after NO_SPEC also produces <name>_spec.json, from which this
# generates <name>.hh (the contract's custom types, via XDR; empty if it has
# none) and <name>.h (its interface, which includes <name>.hh). NO_SPEC is for
# contracts without the #[contract] macro, which have no spec.
#
# Defines <group>_riscv_contracts (the artifacts) and <group>_contract_headers
# (the artifacts and the headers).
function(add_contract_group group)
  cmake_parse_arguments(ARG "" "" "NO_SPEC" ${ARGN})

  set(outputs)
  set(commands)
  set(headers)
  set(xdr_targets)
  set(depends
    ${CMAKE_CURRENT_SOURCE_DIR}/build_risc.sh
    ${CMAKE_SOURCE_DIR}/scripts/thruput-tools-version
    ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.lock
    ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml)

  foreach(path IN LISTS ARG_UNPARSED_ARGUMENTS ARG_NO_SPEC)
    cmake_path(GET path FILENAME name)
    set(out ${CMAKE_CURRENT_BINARY_DIR}/${name})

    # A generated source may not exist yet when the glob runs, so add any
    # that belong to this contract explicitly.
    set(src ${CMAKE_CURRENT_SOURCE_DIR}/${path}/src)
    file(GLOB sources ${src}/*.rs)
    foreach(generated IN LISTS generated_rs_files)
      cmake_path(IS_PREFIX src ${generated} in_src)
      if(in_src)
        list(APPEND sources ${generated})
      endif()
    endforeach()
    list(APPEND depends ${CMAKE_CURRENT_SOURCE_DIR}/${path}/Cargo.toml ${sources})
    list(APPEND outputs ${out}.rv32a ${out}.rv32.json)

    if(path IN_LIST ARG_NO_SPEC)
      list(APPEND commands
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/build_risc.sh --no-spec ${name} ${CMAKE_CURRENT_BINARY_DIR})
    else()
      list(APPEND commands
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/build_risc.sh ${name} ${CMAKE_CURRENT_BINARY_DIR})
      list(APPEND outputs ${out}_spec.json)

      add_custom_command(
        OUTPUT ${out}.x
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/generate_types_xdr.py
          ${out}_spec.json -o ${out}.x -n ${name}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/generate_types_xdr.py ${out}_spec.json
        COMMENT "Generating ${name}.x from spec type_defs"
      )
      add_xdr(${name}_hh ./${name}.x)
      list(APPEND xdr_targets build_${name}_hh)

      add_custom_command(
        OUTPUT ${out}.h
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/generate_header.py
          ${out}_spec.json -o ${out}.h -n ${name} -r ${name}.rv32a
          -i ${CURRENT_RELATIVE_SOURCE_DIR}/${name}.hh
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/generate_header.py ${out}_spec.json
        COMMENT "Generating ${name}.h from spec"
      )
      list(APPEND headers ${out}.h)
    endif()
  endforeach()

  add_custom_command(
    OUTPUT ${outputs}
    ${commands}
    DEPENDS ${depends}
    COMMENT "Building ${group} RV32 contracts via build_risc.sh"
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  )
  add_custom_target(${group}_riscv_contracts DEPENDS ${outputs})

  # The spec-derived steps run after the contract build that produces the
  # specs.
  foreach(xdr_target IN LISTS xdr_targets)
    add_dependencies(${xdr_target} ${group}_riscv_contracts)
  endforeach()
  add_custom_target(${group}_contract_headers DEPENDS ${headers})
  add_dependencies(${group}_contract_headers ${group}_riscv_contracts ${xdr_targets})
endfunction()

# Contracts are in two directories: integration_test_contracts (a submodule)
# and local_test_contracts (checked into this repo directly).
#
# core: what the node binaries need. The precompiles and genesis compile
# against these headers, and releases ship genesis_root.rv32a. Release builds
# only build this group.
add_contract_group(core
  integration_test_contracts/default_wallet
  integration_test_contracts/genesis_root
  integration_test_contracts/itc_erc20
  local_test_contracts/itc_erc20_permit
)

# test: contracts only tests, benchmarks and loadgen use.
#
# Not built at all:
#   - calldata_raw, sha256_raw, fibonacci_raw: no #[contract] macro, and
#     nothing loads their artifacts.
#   - nocontract_wallet: client-stub-only crate with no [[bin]] wrapper, so
#     there is no RV32 entry point to lower.
add_contract_group(test
  integration_test_contracts/authclaim_test
  integration_test_contracts/calldata_wrapper
  integration_test_contracts/cxs_roundtrip
  integration_test_contracts/delegate_call_test
  integration_test_contracts/deploy_and_transfer
  integration_test_contracts/ed25519_verify_wrapper
  integration_test_contracts/fibonacci
  integration_test_contracts/fibonacci_call
  integration_test_contracts/htlc
  integration_test_contracts/manual_revert_caller
  integration_test_contracts/manual_revert_test
  integration_test_contracts/nnint_output_fault_test
  integration_test_contracts/no_syscall_test
  integration_test_contracts/output_alias_test
  integration_test_contracts/payment_channel
  integration_test_contracts/read_only_test
  integration_test_contracts/return_fault_test
  integration_test_contracts/sha256_wrapper
  integration_test_contracts/simple_wallet
  integration_test_contracts/simple_wallet_authclaim
  integration_test_contracts/syscall_misc_tests
  integration_test_contracts/syscall_parse_hardening_test
  integration_test_contracts/tuple_return
  integration_test_contracts/txs_roundtrip
  integration_test_contracts/view_gateway_test
  local_test_contracts/airdrop_faucet
  local_test_contracts/auth_weight_test
  local_test_contracts/drip_faucet
  local_test_contracts/erc165_probe
  local_test_contracts/faucet_caller
  local_test_contracts/invoke_malformed_tokens_test
  local_test_contracts/storage_reads
  local_test_contracts/tx_context_test
  NO_SPEC
    integration_test_contracts/hash_bug
)
add_dependencies(test_contract_headers build_roundtrip_hh)

# Both groups build in the same cargo target directory. Order them so one isn't
# left holding a build slot while it waits on the other's cargo lock.
add_dependencies(test_riscv_contracts core_riscv_contracts)

# Everything. Tests depend on these; all_riscvs_built is another name for
# all_riscv_contracts.
add_custom_target(all_riscv_contracts)
add_dependencies(all_riscv_contracts core_riscv_contracts test_riscv_contracts)
add_custom_target(all_riscvs_built)
add_dependencies(all_riscvs_built all_riscv_contracts)
add_custom_target(all_contract_headers)
add_dependencies(all_contract_headers core_contract_headers test_contract_headers)

add_custom_target(build_all_contracts ALL)
add_dependencies(build_all_contracts all_contract_headers)
