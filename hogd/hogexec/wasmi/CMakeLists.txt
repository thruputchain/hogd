set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/hogd/hogexec/wasmi/src/ffi_enums.rs
    PROPERTIES GENERATED TRUE)

# Import targets defined in a package or workspace manifest `Cargo.toml` file
corrosion_import_crate(MANIFEST_PATH ./Cargo.toml CRATE_TYPES staticlib)

target_sources(
  wasmi_lib
  PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ffi_enums.rs)

add_dependencies(
  wasmi_lib
  ffi_enum_rs
)

target_link_libraries(
  wasmi_lib
  INTERFACE
  runtime_trampoline
)

