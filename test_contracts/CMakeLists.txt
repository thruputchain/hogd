if(ENABLE_TEST_CONTRACTS)

function(add_wasm_contract target_name)

  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.wasm_unoptimized
    COMMAND CARGO_NET_GIT_FETCH_WITH_CLI=true
        RUSTFLAGS='-C link-args=-zstack-size=30000'
        cargo +nightly build --locked
        -p ${target_name}
        --target wasm32-unknown-unknown
        --release
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

  wasmopt_target(${target_name}_optimized ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.wasm_unoptimized ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.wasm -mvp --enable-bulk-memory -Oz)

  add_custom_target(${target_name}_wasm ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${target_name}.wasm)
endfunction()

add_wasm_contract(fibonacci)

add_custom_target(all_wasm_contracts DEPENDS fibonacci_wasm)


else()
  add_custom_target(all_wasm_contracts)
endif()