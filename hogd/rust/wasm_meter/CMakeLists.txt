add_custom_command(
    OUTPUT
        ${CMAKE_CURRENT_BINARY_DIR}/bindings.h
    DEPENDS
      ${CMAKE_CURRENT_SOURCE_DIR}/src/lib.rs
      ${CMAKE_CURRENT_SOURCE_DIR}/../Cargo.lock
    COMMAND
      cbindgen --config cbindgen.toml --crate wasm_meter --output ${CMAKE_CURRENT_BINARY_DIR}/bindings.h
    WORKING_DIRECTORY
      ${CMAKE_CURRENT_SOURCE_DIR}
)

add_custom_target(
	generate_wasm_meter_ffi_h ALL
	DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/bindings.h
)
