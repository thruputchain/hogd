

add_custom_command(
    OUTPUT
        ${CMAKE_CURRENT_BINARY_DIR}/bindings.h
    DEPENDS 
      ${CMAKE_CURRENT_SOURCE_DIR}/src/lib.rs
      ${CMAKE_CURRENT_SOURCE_DIR}/src/siphash24.rs
      ${CMAKE_CURRENT_SOURCE_DIR}/src/ed25519.rs
    COMMAND 
      cbindgen --config cbindgen.toml --crate crypto_lib --output ${CMAKE_CURRENT_BINARY_DIR}/bindings.h
    WORKING_DIRECTORY
      ${CMAKE_CURRENT_SOURCE_DIR}
)

add_custom_target(
	generate_crypto_ffi_h ALL
	DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/bindings.h
)
