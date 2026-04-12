# Get the relative path of the current source directory
cmake_path(RELATIVE_PATH CMAKE_CURRENT_SOURCE_DIR BASE_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE CURRENT_RELATIVE_SOURCE_DIR)

# Add contract loader library
add_library(contract_loader
  contract_loader.cc
  ${CMAKE_SOURCE_DIR}/hogd/hogexec/runtime/wasm/instrumented_bytes.cc
)

target_link_libraries(contract_loader
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

target_compile_definitions(contract_loader PRIVATE
  CMAKE_ROOT_BINARY_DIR="${CMAKE_BINARY_DIR}"
)

add_custom_target(all_wasm_contracts)
add_custom_target(all_wasm_headers)
add_dependencies(all_wasm_contracts all_wasm_headers)

# Configure WASM build based on WASM_DEBUG_BACKTRACES flag
if(WASM_DEBUG_BACKTRACES)
  set(WASM_FLAGS "-F logging,entrypoints")
  set(WASM_BUILD_FOLDER "debug")
  set(WASM_CARGO_RELEASE_FLAG "")
  set(WASMOPT_DEBUG_FLAG "-g")
  message(STATUS "WASM contracts: debug build with backtraces enabled")
else()
  set(WASM_FLAGS "-F entrypoints")
  set(WASM_BUILD_FOLDER "release")
  set(WASM_CARGO_RELEASE_FLAG "--release")
  set(WASMOPT_DEBUG_FLAG "")
  message(STATUS "WASM contracts: release build (minimal size, no backtraces)")
endif()

# Tell CMake to clean cargo build directories
set_property(DIRECTORY APPEND PROPERTY ADDITIONAL_CLEAN_FILES
  "${CMAKE_CURRENT_BINARY_DIR}/spec_build"
  "${CMAKE_CURRENT_BINARY_DIR}/wasm32-unknown-unknown"
  "${CMAKE_CURRENT_BINARY_DIR}/release"
  "${CMAKE_CURRENT_BINARY_DIR}/debug"
  "${CMAKE_CURRENT_BINARY_DIR}/.fingerprint"
  "${CMAKE_CURRENT_BINARY_DIR}/incremental"
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

set(roundtrip_rs_stamp ${CMAKE_CURRENT_BINARY_DIR}/roundtrip_rs.stamp)
add_custom_command(
  OUTPUT ${roundtrip_rs_stamp}
  COMMAND ${CMAKE_COMMAND} -E touch ${roundtrip_rs_stamp}
  DEPENDS ${generated_rs_files}
  COMMENT "Roundtrip RS files generated"
)
add_custom_target(build_roundtrip_rs DEPENDS ${roundtrip_rs_stamp})

# Build all WASM targets with separate cargo commands in one custom command
function(add_wasm_builds)
  # ARGN contains all the target names passed to the function
  
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
  
  # Build lists of all outputs and build commands
  set(all_unoptimized_wasms "")
  set(all_optimized_wasms "")
  set(all_depends ${common_depends})
  set(build_commands "")

  foreach(target_name ${ARGN})
    list(APPEND all_unoptimized_wasms ${CMAKE_CURRENT_BINARY_DIR}/wasm32-unknown-unknown/${WASM_BUILD_FOLDER}/${target_name}.wasm)
    list(APPEND all_optimized_wasms ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.wasm)
    
    # Get all .rs files but filter out generated roundtrip.rs
    file(GLOB target_rs_files 
      ${CMAKE_CURRENT_SOURCE_DIR}/integration_test_contracts/${target_name}/src/*.rs)
    list(FILTER target_rs_files EXCLUDE REGEX "roundtrip\\.rs$")
    
    list(APPEND all_depends
      ${CMAKE_CURRENT_SOURCE_DIR}/integration_test_contracts/${target_name}/Cargo.toml
      ${target_rs_files})
    
    # Add individual cargo build command for this target
    list(APPEND build_commands
      COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/build_wasm.sh
        ${target_name}
        ${CMAKE_CURRENT_BINARY_DIR}
        ${WASM_CARGO_RELEASE_FLAG}
        "${WASM_FLAGS}")
  endforeach()
  
  # Single custom command that builds all unoptimized WASMs with separate cargo commands
  add_custom_command(
    OUTPUT ${all_unoptimized_wasms}
    ${build_commands}
    DEPENDS ${all_depends}
    COMMENT "Building all WASM contracts with separate cargo invocations"
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  )
  
  # Create one target for all unoptimized WASMs
  add_custom_target(all_unoptimized_wasms DEPENDS ${all_unoptimized_wasms})

  # Run wasm-opt on each WASM individually
  foreach(target_name ${ARGN})
    wasmopt_target(
      ${target_name}_optimized
      ${CMAKE_CURRENT_BINARY_DIR}/wasm32-unknown-unknown/${WASM_BUILD_FOLDER}/${target_name}.wasm
      ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.wasm
      -mvp --enable-bulk-memory --enable-sign-ext --enable-mutable-globals -Oz ${WASMOPT_DEBUG_FLAG})
    
    # Make the optimized target depend on the shared unoptimized build
    add_dependencies(${target_name}_optimized all_unoptimized_wasms)
    
    # Create target for each optimized WASM (NOT marked ALL)
    add_custom_target(${target_name}_wasm DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.wasm)
    add_dependencies(${target_name}_wasm ${target_name}_optimized)
  endforeach()
  
  # Create ONE target that builds everything (but wait for it to be marked ALL after headers are set up)
  add_custom_target(all_wasms_built DEPENDS ${all_optimized_wasms})
  add_dependencies(all_wasm_contracts all_wasms_built)
endfunction()

# Build all spec-related binaries (spec JSONs and types XDR) in one cargo invocation
# Uses a separate target directory to avoid conflicts with WASM builds
# Usage: add_spec_and_types_generation(
#   SPEC_TARGETS target1 target2 ...
#   TYPES_TARGETS target3 target4 ...
# )
function(add_spec_and_types_generation)
  # Parse named arguments
  cmake_parse_arguments(ARG "" "" "SPEC_TARGETS;TYPES_TARGETS" ${ARGN})
  
  set(all_outputs "")
  set(all_depends "")
  set(all_bins "")
  
  # Build lists for spec targets
  foreach(target_name ${ARG_SPEC_TARGETS})
    list(APPEND all_outputs ${CMAKE_CURRENT_BINARY_DIR}/${target_name}_spec.json)
    
    # Get all .rs files but filter out generated roundtrip.rs
    file(GLOB target_src_rs_files 
      ${CMAKE_CURRENT_SOURCE_DIR}/integration_test_contracts/${target_name}/src/*.rs)
    list(FILTER target_src_rs_files EXCLUDE REGEX "roundtrip\\.rs$")
    file(GLOB target_bin_rs_files 
      ${CMAKE_CURRENT_SOURCE_DIR}/integration_test_contracts/${target_name}/src/bin/*.rs)
    
    list(APPEND all_depends 
      ${CMAKE_CURRENT_SOURCE_DIR}/integration_test_contracts/${target_name}/Cargo.toml
      ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml
      ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.lock
      ${target_src_rs_files}
      ${target_bin_rs_files}
      ${CMAKE_CURRENT_SOURCE_DIR}/build_wasm.sh)
    list(APPEND all_bins "--bin" "generate_spec_${target_name}")
  endforeach()
  
  # Build lists for types targets
  foreach(target_name ${ARG_TYPES_TARGETS})
    list(APPEND all_outputs ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.x)
    
    # Get all .rs files but filter out generated roundtrip.rs
    file(GLOB target_src_rs_files 
      ${CMAKE_CURRENT_SOURCE_DIR}/integration_test_contracts/${target_name}/src/*.rs)
    list(FILTER target_src_rs_files EXCLUDE REGEX "roundtrip\\.rs$")
    file(GLOB target_bin_rs_files 
      ${CMAKE_CURRENT_SOURCE_DIR}/integration_test_contracts/${target_name}/src/bin/*.rs)
    
    list(APPEND all_depends 
      ${CMAKE_CURRENT_SOURCE_DIR}/integration_test_contracts/${target_name}/Cargo.toml
      ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml
      ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.lock
      ${target_src_rs_files}
      ${target_bin_rs_files})
    list(APPEND all_bins "--bin" "generate_types_${target_name}")
  endforeach()
  
  # Add common dependencies (use stamp file to ensure roundtrip.rs files exist)
  list(APPEND all_depends 
    ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.lock
    ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml
    ${roundtrip_rs_stamp})
  
  # Build the generation commands list first
  set(gen_commands "")
  foreach(target_name ${ARG_SPEC_TARGETS})
    list(APPEND gen_commands 
      COMMAND ${CMAKE_CURRENT_BINARY_DIR}/spec_build/release/generate_spec_${target_name}
              ${CMAKE_CURRENT_BINARY_DIR}/${target_name}_spec.json)
  endforeach()
  
  foreach(target_name ${ARG_TYPES_TARGETS})
    list(APPEND gen_commands
      COMMAND ${CMAKE_CURRENT_BINARY_DIR}/spec_build/release/generate_types_${target_name}
              ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.x
              ${target_name})
  endforeach()
  
  # Use a stamp file to ensure this runs exactly once
  set(stamp_file ${CMAKE_CURRENT_BINARY_DIR}/all_specs_and_types.stamp)
  
  # Single custom command that builds all binaries and generates all outputs
  add_custom_command(
    OUTPUT ${stamp_file}
    # First, build all spec/types binaries in one cargo invocation
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/build_wasm_spec.sh ${CMAKE_CURRENT_BINARY_DIR}/spec_build ${all_bins}
    # Then run each binary to generate its output
    ${gen_commands}
    COMMAND ${CMAKE_COMMAND} -E touch ${stamp_file}
    DEPENDS ${all_depends}
    COMMENT "Building all spec/types binaries and generating outputs in one step"
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  )
  
  # Create ONE target (NOT ALL) that depends on the stamp file
  add_custom_target(all_spec_and_types DEPENDS ${stamp_file})
  
  foreach(target_name ${ARG_TYPES_TARGETS})
    add_xdr(${target_name}_hh ./${target_name}.x)
    # Make sure XDR generation waits for the .x file to be generated
    add_dependencies(build_${target_name}_hh all_spec_and_types)
  endforeach()
endfunction()

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
  # Ensure spec is generated before trying to generate header from it
  add_dependencies(${target_name}_header all_spec_and_types)

  # make sure that other targets can depend on the generated headers,
  # not just the wasms
  add_dependencies(all_wasm_headers ${target_name}_header)
endfunction()

# Build all WASMs in one cargo invocation
add_wasm_builds(
  fibonacci
  fibonacci_call
  cxs_roundtrip
  txs_roundtrip
  read_only_test
  manual_revert_test
  manual_revert_caller
  simple_wallet
  simple_wallet_authclaim
  sha256_wrapper
  initial_token_claim
  itc_erc20
  authclaim_test
  hash_bug
  htlc
  payment_channel
  return_fault_test
  tuple_return
  no_syscall_test
  syscall_misc_tests
  syscall_parse_hardening_test
  nnint_output_fault_test
  output_alias_test
)

# Build all spec JSONs and types XDR in one cargo invocation (separate target-dir to avoid conflicts)
add_spec_and_types_generation(
  SPEC_TARGETS
    fibonacci
    fibonacci_call
    cxs_roundtrip
    txs_roundtrip
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
  TYPES_TARGETS
    itc_erc20
    manual_revert_test
    txs_roundtrip
    cxs_roundtrip
    payment_channel
)

# Make all headers wait for all specs to be generated
add_dependencies(all_wasm_headers all_spec_and_types)

# Generate contract headers
add_contract_header(fibonacci)
add_contract_header(fibonacci_call)
add_contract_header(cxs_roundtrip "${CURRENT_RELATIVE_SOURCE_DIR}/cxs_roundtrip.hh")
add_contract_header(txs_roundtrip "${CURRENT_RELATIVE_SOURCE_DIR}/txs_roundtrip.hh")
add_contract_header(read_only_test)
add_contract_header(manual_revert_test "${CURRENT_RELATIVE_SOURCE_DIR}/manual_revert_test.hh")
add_contract_header(manual_revert_caller)
add_contract_header(sha256_wrapper)
add_contract_header(simple_wallet)
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

add_dependencies(itc_erc20_header itc_erc20_hh)
add_dependencies(payment_channel_header payment_channel_hh)
add_dependencies(cxs_roundtrip_header cxs_roundtrip_hh)
add_dependencies(txs_roundtrip_header txs_roundtrip_hh)
add_dependencies(manual_revert_test_header manual_revert_test_hh)

# Create ONE top-level ALL target that triggers everything
add_custom_target(build_all_contracts ALL)
add_dependencies(build_all_contracts all_wasm_contracts all_spec_and_types)
