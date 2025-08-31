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

function(add_wasm_build target_name)

  # If you update the contracts (i.e. add new contracts, or change dependencies)
  # then Cargo.lock will change, and the build will fail.  If that happens,
  # just comment out the --locked flag, and the Cargo.lock will be regenerated.
  # The --locked flag forces the CI to build with the correct version of the sdk
  # (not just whatever the latest version it has cached is).
  # Remember to put the --locked flag back in when you're done.
  # 
  # The better alternative is to run cargo +nightly update to update the lockfile manually.
  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.wasm_unoptimized
    COMMAND CARGO_NET_GIT_FETCH_WITH_CLI=true
        RUSTFLAGS='-C link-args=-zstack-size=30000'
        cargo +nightly build
        -p ${target_name}
        --target wasm32-unknown-unknown
        --release
        # --locked
        --manifest-path ${CMAKE_CURRENT_SOURCE_DIR}/integration_test_contracts/${target_name}/Cargo.toml
        --target-dir ${CMAKE_CURRENT_BINARY_DIR} 
        -Zbuild-std=core,alloc
        -Zbuild-std-features=panic_immediate_abort &&
        cp 
          ${CMAKE_CURRENT_BINARY_DIR}/wasm32-unknown-unknown/release/${target_name}.wasm 
          ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.wasm_unoptimized
    DEPENDS 
        ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.lock
        ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml
        ${CMAKE_CURRENT_SOURCE_DIR}/integration_test_contracts/${target_name}/Cargo.toml 
        ${CMAKE_CURRENT_SOURCE_DIR}/integration_test_contracts/${target_name}/src/*.rs
    COMMENT "Building ${target_name}.wasm_unoptimized"
  )

  wasmopt_target(
    ${target_name}_optimized 
    ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.wasm_unoptimized 
    ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.wasm 
    -mvp --enable-bulk-memory --enable-sign-ext --enable-mutable-globals -Oz)

  add_custom_target(${target_name}_wasm ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.wasm)

  add_dependencies(all_wasm_contracts ${target_name}_wasm)
endfunction()

function(add_spec_json target_name)
  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${target_name}_spec.json
    COMMAND CARGO_NET_GIT_FETCH_WITH_CLI=true
        cargo +nightly run
        --features spec
        #--locked
        --manifest-path ${CMAKE_CURRENT_SOURCE_DIR}/integration_test_contracts/${target_name}/Cargo.toml
        --target-dir ${CMAKE_CURRENT_BINARY_DIR}
        --bin generate_spec
        -- ${CMAKE_CURRENT_BINARY_DIR}/${target_name}_spec.json
    DEPENDS 
        ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.lock
        ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml
        ${CMAKE_CURRENT_SOURCE_DIR}/integration_test_contracts/${target_name}/Cargo.toml 
        ${CMAKE_CURRENT_SOURCE_DIR}/integration_test_contracts/${target_name}/src/*.rs
    COMMENT "Generating ${target_name}_spec.json"
  )

  add_custom_target(${target_name}_spec_json ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${target_name}_spec.json)
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

function(add_types_xdr target_name)
  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.x
    COMMAND CARGO_NET_GIT_FETCH_WITH_CLI=true
        cargo +nightly run
        --features spec
        --locked
        --manifest-path ${CMAKE_CURRENT_SOURCE_DIR}/integration_test_contracts/${target_name}/Cargo.toml
        --target-dir ${CMAKE_CURRENT_BINARY_DIR}
        --bin generate_types
        -- ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.x
        ${target_name}
    DEPENDS 
        ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.lock
        ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml
        ${CMAKE_CURRENT_SOURCE_DIR}/integration_test_contracts/${target_name}/Cargo.toml 
        ${CMAKE_CURRENT_SOURCE_DIR}/integration_test_contracts/${target_name}/src/*.rs
    COMMENT "Generating ${target_name}.x"
  )

  add_xdr(${target_name}_hh ./${target_name}.x)

  add_custom_target(${target_name}_types_xdr ALL DEPENDS ${target_name}_hh)
endfunction()

function(add_wasm_contract target_name)
  add_wasm_build(${target_name})
  add_spec_json(${target_name})
  add_contract_header(${target_name} ${ARGN})
endfunction()

add_wasm_contract(fibonacci)
add_wasm_contract(fibonacci_call)
add_wasm_contract(cxs_roundtrip)
add_wasm_contract(txs_roundtrip)
add_wasm_contract(read_only_test)
add_wasm_contract(manual_revert_test)
add_wasm_contract(manual_revert_caller)
add_wasm_contract(simple_wallet)
add_wasm_contract(initial_token_claim)

# TODO(geoff): make a cleaner way to automate dependency tracking on these headers
add_types_xdr(itc_erc20)
add_wasm_contract(itc_erc20 "${CURRENT_RELATIVE_SOURCE_DIR}/itc_erc20.hh")
add_dependencies(itc_erc20_header itc_erc20_types_xdr)

add_xdr_rs(
	cxs_roundtrip_rs 
	${CMAKE_CURRENT_SOURCE_DIR}/integration_test_contracts/cxs_roundtrip/src
	./integration_test_contracts/xdr/roundtrip.x)

add_xdr_rs(
	txs_roundtrip_rs 
	${CMAKE_CURRENT_SOURCE_DIR}/integration_test_contracts/txs_roundtrip/src
	./integration_test_contracts/xdr/roundtrip.x)

add_xdr(roundtrip_hh
  ./integration_test_contracts/xdr/roundtrip.x
)

add_dependencies(cxs_roundtrip_wasm cxs_roundtrip_rs)
add_dependencies(cxs_roundtrip_spec_json cxs_roundtrip_rs)

add_dependencies(txs_roundtrip_wasm txs_roundtrip_rs)
add_dependencies(txs_roundtrip_spec_json txs_roundtrip_rs)

add_dependencies(all_wasm_contracts roundtrip_hh)
