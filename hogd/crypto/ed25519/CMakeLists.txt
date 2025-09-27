# Import targets defined in a package or workspace manifest `Cargo.toml` file
corrosion_import_crate(MANIFEST_PATH ./Cargo.toml CRATE_TYPES staticlib LOCKED FROZEN)
# To enable an online build, set CARGO_NET_GIT_FETCH_WITH_CLI=true and remove CARGO_NET_OFFLINE=true
corrosion_set_env_vars(ed25519_lib CARGO_NET_OFFLINE=true)
corrosion_add_target_rustflags(ed25519_lib "-C target-cpu=native")

add_custom_command(
    OUTPUT
        ${CMAKE_CURRENT_BINARY_DIR}/bindings.h
    DEPENDS 
      ${CMAKE_CURRENT_SOURCE_DIR}/src/lib.rs
    COMMAND 
      cbindgen --config cbindgen.toml --crate ed25519_lib --output ${CMAKE_CURRENT_BINARY_DIR}/bindings.h
    WORKING_DIRECTORY
      ${CMAKE_CURRENT_SOURCE_DIR}
)

add_custom_target(
	generate_ed25519_ffi_h ALL
	DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/bindings.h
)

add_dependencies(
  generate_ed25519_ffi_h
  cargo-prebuild_ed25519_lib
)
