set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/hogd/hogexec/wasmi/src/ffi_enums.rs
    PROPERTIES GENERATED TRUE)

# Import targets defined in a package or workspace manifest `Cargo.toml` file
corrosion_import_crate(MANIFEST_PATH ./Cargo.toml CRATE_TYPES staticlib LOCKED)
corrosion_set_env_vars(wasmi_lib CARGO_NET_GIT_FETCH_WITH_CLI=true)

add_dependencies(
  cargo-prebuild_wasmi_lib
  ffi_enum_rs)

target_sources(
  wasmi_lib
  PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ffi_enums.rs)

target_link_libraries(
  wasmi_lib
  INTERFACE
  runtime_trampoline
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  corrosion_set_features(wasmi_lib
      FEATURES
          debug_logging
  )
endif()

add_custom_command(
    OUTPUT
        ${CMAKE_CURRENT_BINARY_DIR}/bindings.h
    DEPENDS 
      ${CMAKE_CURRENT_SOURCE_DIR}/src/context.rs
      ${CMAKE_CURRENT_SOURCE_DIR}/src/external_call.rs
      ${CMAKE_CURRENT_SOURCE_DIR}/src/invoke_result.rs
      ${CMAKE_CURRENT_SOURCE_DIR}/src/lib.rs
      ${CMAKE_CURRENT_SOURCE_DIR}/src/module_cache.rs
      ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime.rs
    COMMAND 
      cbindgen --config cbindgen.toml --crate wasmi_lib --output ${CMAKE_CURRENT_BINARY_DIR}/bindings.h
    WORKING_DIRECTORY
      ${CMAKE_CURRENT_SOURCE_DIR}
)

add_custom_target(
	generate_wasmi_ffi_h ALL
	DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/bindings.h
)

add_dependencies(
  generate_wasmi_ffi_h
  ffi_enum_rs
)

