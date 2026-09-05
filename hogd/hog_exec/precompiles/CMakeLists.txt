add_library(precompiles_objects OBJECT
  context.cc
  native_contract_builder.cc
  precompile_native_token.cc
  precompile_default_wallet.cc
)

target_link_libraries(
  precompiles_objects
  hogd_interface
  components_library_exception
)

add_library(precompiles STATIC $<TARGET_OBJECTS:precompiles_objects>)

target_include_directories(
  precompiles_objects
  PRIVATE
  ${CMAKE_BINARY_DIR}
)

add_dependencies(
    precompiles_objects 
    all_contract_headers
    generate_syscall_inner_h
    generate_syscall_meta_h
)

target_link_libraries(
  precompiles
  runtime
)
