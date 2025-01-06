# Import targets defined in a package or workspace manifest `Cargo.toml` file
corrosion_import_crate(MANIFEST_PATH ./Cargo.toml CRATE_TYPES staticlib)

target_link_libraries(
  wasmi_lib
  INTERFACE
  runtime_trampoline
)

set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/hogd/hogexec/wasmi/src/ffi_enums.rs
    PROPERTIES GENERATED TRUE)

add_dependencies(
  wasmi_lib
  ${CMAKE_SOURCE_DIR}/hogd/hogexec/wasmi/src/ffi_enums.rs
  ffi_enum_rs
)

