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

function(add_wasm_contract target_name)

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

add_wasm_contract(fibonacci)
add_wasm_contract(fibonacci_call)
add_wasm_contract(cxs_roundtrip)
add_wasm_contract(read_only_test)
add_wasm_contract(manual_revert_test)
add_wasm_contract(manual_revert_caller)
add_wasm_contract(simple_wallet)
add_wasm_contract(initial_token_claim)

add_xdr_rs(
	roundtrip_rs 
	${CMAKE_CURRENT_SOURCE_DIR}/integration_test_contracts/cxs_roundtrip/src
	./integration_test_contracts/cxs_roundtrip/roundtrip.x)

add_xdr(roundtrip_hh
  ./integration_test_contracts/cxs_roundtrip/roundtrip.x
)

add_dependencies(cxs_roundtrip_wasm roundtrip_rs)
add_dependencies(all_wasm_contracts roundtrip_hh)