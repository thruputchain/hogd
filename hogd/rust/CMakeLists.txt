add_subdirectory(crypto)
add_subdirectory(wasmi)

set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/wasmi/src/ffi_enums.rs
    PROPERTIES GENERATED TRUE)

# Import targets defined in the workspace manifest `Cargo.toml` file
corrosion_import_crate(MANIFEST_PATH ./Cargo.toml CRATES unified_lib CRATE_TYPES staticlib LOCKED FROZEN)
# To enable an online build, set CARGO_NET_GIT_FETCH_WITH_CLI=true and remove CARGO_NET_OFFLINE=true
corrosion_set_env_vars(unified_lib CARGO_NET_OFFLINE=true)

corrosion_add_target_rustflags(unified_lib "-C target-cpu=native")



if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  corrosion_set_features(unified_lib
      FEATURES
          debug_logging
  )
endif()


add_dependencies(
  cargo-prebuild_unified_lib
  ffi_enum_rs)

target_sources(
  unified_lib
  PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/wasmi/src/ffi_enums.rs)

target_link_libraries(
  unified_lib
  INTERFACE
  runtime_trampoline
)