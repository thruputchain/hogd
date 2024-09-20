# Import targets defined in a package or workspace manifest `Cargo.toml` file
corrosion_import_crate(MANIFEST_PATH ./Cargo.toml CRATE_TYPES staticlib)

target_link_libraries(
  wasmi_lib
  INTERFACE
  runtime_trampoline
)
