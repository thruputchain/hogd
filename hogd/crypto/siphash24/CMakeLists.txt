# Import targets defined in a package or workspace manifest `Cargo.toml` file
corrosion_import_crate(MANIFEST_PATH ./Cargo.toml CRATE_TYPES staticlib LOCKED FROZEN)
# To enable an online build, set CARGO_NET_GIT_FETCH_WITH_CLI=true and remove CARGO_NET_OFFLINE=true
corrosion_set_env_vars(siphash24_lib CARGO_NET_OFFLINE=true)
corrosion_add_target_rustflags(siphash24_lib "-C target-cpu=native")

add_custom_command(
    OUTPUT
        ${CMAKE_CURRENT_BINARY_DIR}/bindings.h
    DEPENDS 
      ${CMAKE_CURRENT_SOURCE_DIR}/src/lib.rs
    COMMAND 
      cbindgen --config cbindgen.toml --crate siphash24_lib --output ${CMAKE_CURRENT_BINARY_DIR}/bindings.h
    WORKING_DIRECTORY
      ${CMAKE_CURRENT_SOURCE_DIR}
)

add_custom_target(
	generate_siphash24_ffi_h ALL
	DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/bindings.h
)

add_dependencies(
  generate_siphash24_ffi_h
  cargo-prebuild_siphash24_lib
)
