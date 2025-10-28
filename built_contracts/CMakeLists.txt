# Get the relative path of the current source directory
cmake_path(RELATIVE_PATH CMAKE_CURRENT_SOURCE_DIR BASE_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE CURRENT_RELATIVE_SOURCE_DIR)

# Add contract loader library
add_library(contract_loader
  contract_loader.cc
)

target_link_libraries(contract_loader
  x23
  protocol
)

target_include_directories(contract_loader PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/hogd/xdr
)

target_compile_definitions(contract_loader PRIVATE
  CMAKE_ROOT_BINARY_DIR="${CMAKE_BINARY_DIR}"
)

add_custom_target(all_wasm_contracts)
add_custom_target(all_wasm_headers)
add_dependencies(all_wasm_contracts all_wasm_headers)

set(WASM_FLAGS "")
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(WASM_FLAGS "-F logging")
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

# Build each WASM target separately (building together causes larger binaries)
# but use a shared stamp file to serialize builds and avoid cargo conflicts
function(add_wasm_builds)
  # ARGN contains all the target names passed to the function
  
  # Common dependencies
  set(common_depends 
    ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.lock
    ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml
    ${generated_rs_files})
  
  # If you update the contracts (i.e. add new contracts, or change dependencies)
  # then Cargo.lock will change, and the build will fail.  If that happens,
  # run cargo update to update the lockfile manually.
  #
  # This will not fetch crates during the build process; you need to run the fetch script
  # `./scripts/fetch.sh`
  # if your set of downloaded crates is incomplete.
  
  # Use a stamp file to serialize cargo invocations and prevent conflicts
  set(prev_stamp "")
  set(prev_target "")
  foreach(target_name ${ARGN})
    set(stamp_file ${CMAKE_CURRENT_BINARY_DIR}/wasm_build_${target_name}.stamp)
    
    set(target_depends
      ${CMAKE_CURRENT_SOURCE_DIR}/integration_test_contracts/${target_name}/Cargo.toml
      ${CMAKE_CURRENT_SOURCE_DIR}/integration_test_contracts/${target_name}/src/*.rs
      ${common_depends})
    
    # Make each build depend on the previous one to serialize cargo invocations
    if(prev_stamp)
      list(APPEND target_depends ${prev_stamp})
    endif()
    
    add_custom_command(
      OUTPUT ${stamp_file} ${CMAKE_CURRENT_BINARY_DIR}/wasm32-unknown-unknown/release/${target_name}.wasm
      COMMAND env CARGO_NET_GIT_FETCH_WITH_CLI=true RUSTFLAGS='-C link-args=-zstack-size=30000 -Zunstable-options -Cpanic=immediate-abort'
          cargo build
          -p ${target_name}
          --target wasm32-unknown-unknown
          --release
          --locked
          --frozen
          ${WASM_FLAGS}
          --offline
          --target-dir ${CMAKE_CURRENT_BINARY_DIR} 
          -Zbuild-std=core,alloc
          -Zbuild-std-features=std/optimize_for_size
      COMMAND ${CMAKE_COMMAND} -E touch ${stamp_file}
      DEPENDS ${target_depends}
      COMMENT "Building ${target_name}.wasm"
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    
    # Create an intermediate target for the unoptimized wasm (cargo build output)
    add_custom_target(${target_name}_unoptimized DEPENDS 
      ${CMAKE_CURRENT_BINARY_DIR}/wasm32-unknown-unknown/release/${target_name}.wasm
      ${stamp_file})
    
    # Enforce target-level dependency to prevent parallel cargo builds
    # This serializes the actual cargo invocations, not the wasm-opt steps
    if(prev_target)
      add_dependencies(${target_name}_unoptimized ${prev_target})
    endif()
    
    wasmopt_target(
      ${target_name}_optimized 
      ${CMAKE_CURRENT_BINARY_DIR}/wasm32-unknown-unknown/release/${target_name}.wasm
      ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.wasm 
      -mvp --enable-bulk-memory --enable-sign-ext --enable-mutable-globals -Oz)
    
    # Make the optimized target depend on the unoptimized one
    add_dependencies(${target_name}_optimized ${target_name}_unoptimized)

    add_custom_target(${target_name}_wasm ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.wasm)

    add_dependencies(all_wasm_contracts ${target_name}_wasm)
    
    set(prev_stamp ${stamp_file})
    set(prev_target ${target_name}_unoptimized)
  endforeach()
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
    list(APPEND all_depends 
      ${CMAKE_CURRENT_SOURCE_DIR}/integration_test_contracts/${target_name}/Cargo.toml
      ${CMAKE_CURRENT_SOURCE_DIR}/integration_test_contracts/${target_name}/src/*.rs)
    list(APPEND all_bins "--bin" "generate_spec_${target_name}")
  endforeach()
  
  # Build lists for types targets
  foreach(target_name ${ARG_TYPES_TARGETS})
    list(APPEND all_outputs ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.x)
    list(APPEND all_depends 
      ${CMAKE_CURRENT_SOURCE_DIR}/integration_test_contracts/${target_name}/Cargo.toml
      ${CMAKE_CURRENT_SOURCE_DIR}/integration_test_contracts/${target_name}/src/*.rs)
    list(APPEND all_bins "--bin" "generate_types_${target_name}")
  endforeach()
  
  list(APPEND all_depends ${generated_rs_files})
  
  # Add common dependencies
  list(APPEND all_depends 
    ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.lock
    ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml)
  
  # Use a stamp file to ensure cargo only runs once, then generate all outputs
  set(stamp_file ${CMAKE_CURRENT_BINARY_DIR}/spec_build.stamp)
  
  add_custom_command(
    OUTPUT ${stamp_file}
    # First, build all spec/types binaries in one cargo invocation
    # Use separate target-dir to avoid conflicts with WASM builds
    COMMAND CARGO_NET_GIT_FETCH_WITH_CLI=true
        cargo build
        --features spec
        --locked
        --release
        --frozen
        --offline
        --target-dir ${CMAKE_CURRENT_BINARY_DIR}/spec_build
        ${all_bins}
    COMMAND ${CMAKE_COMMAND} -E touch ${stamp_file}
    DEPENDS ${all_depends}
    COMMENT "Building all spec/types binaries in one cargo invocation"
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  )
  
  # Generate each spec JSON by running its binary
  foreach(target_name ${ARG_SPEC_TARGETS})
    add_custom_command(
      OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${target_name}_spec.json
      COMMAND ${CMAKE_CURRENT_BINARY_DIR}/spec_build/release/generate_spec_${target_name}
              ${CMAKE_CURRENT_BINARY_DIR}/${target_name}_spec.json
      DEPENDS ${stamp_file}
      COMMENT "Generating ${target_name}_spec.json"
    )
    add_custom_target(${target_name}_spec_json ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${target_name}_spec.json)
  endforeach()
  
  # Generate each types XDR by running its binary
  foreach(target_name ${ARG_TYPES_TARGETS})
    add_custom_command(
      OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.x
      COMMAND ${CMAKE_CURRENT_BINARY_DIR}/spec_build/release/generate_types_${target_name}
              ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.x
              ${target_name}
      DEPENDS ${stamp_file}
      COMMENT "Generating ${target_name}.x"
    )
    add_xdr(${target_name}_hh ./${target_name}.x)
    add_custom_target(${target_name}_types_xdr ALL DEPENDS ${target_name}_hh)
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
      ${CMAKE_CURRENT_BINARY_DIR}/${target_name}_spec.json
      ${CMAKE_CURRENT_SOURCE_DIR}/generate_header.py
    COMMENT "Generating ${target_name}.h from spec"
  )

  add_custom_target(${target_name}_header ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.h)
  add_dependencies(${target_name}_header ${target_name}_spec_json)

  # make sure that other targets can depend on the generated headers,
  # not just the wasms
  add_dependencies(all_wasm_headers ${target_name}_header)
endfunction()

# Build all WASMs in one cargo invocation
add_wasm_builds(fibonacci fibonacci_call cxs_roundtrip txs_roundtrip read_only_test manual_revert_test manual_revert_caller simple_wallet initial_token_claim itc_erc20)

# Build all spec JSONs and types XDR in one cargo invocation (separate target-dir to avoid conflicts)
add_spec_and_types_generation(
  SPEC_TARGETS fibonacci fibonacci_call cxs_roundtrip txs_roundtrip read_only_test manual_revert_test manual_revert_caller simple_wallet initial_token_claim itc_erc20
  TYPES_TARGETS itc_erc20
)

# Generate contract headers
add_contract_header(fibonacci)
add_contract_header(fibonacci_call)
add_contract_header(cxs_roundtrip)
add_contract_header(txs_roundtrip)
add_contract_header(read_only_test)
add_contract_header(manual_revert_test)
add_contract_header(manual_revert_caller)
add_contract_header(simple_wallet)
add_contract_header(initial_token_claim)
add_contract_header(itc_erc20 "${CURRENT_RELATIVE_SOURCE_DIR}/itc_erc20.hh")

add_dependencies(itc_erc20_header itc_erc20_types_xdr)

add_dependencies(cxs_roundtrip_wasm cxs_roundtrip_rs)
add_dependencies(cxs_roundtrip_spec_json cxs_roundtrip_rs)

add_dependencies(txs_roundtrip_wasm txs_roundtrip_rs)
add_dependencies(txs_roundtrip_spec_json txs_roundtrip_rs)

