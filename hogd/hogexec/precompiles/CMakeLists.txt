add_library(precompiles_objects OBJECT
  context.cc
  native_contract_builder.cc
  precompile_native_token.cc
)

target_link_libraries(precompiles_objects
  hogd_interface
)

add_library(precompiles STATIC $<TARGET_OBJECTS:precompiles_objects>)

target_include_directories(
  precompiles_objects
  PRIVATE
  ${CMAKE_BINARY_DIR}
)

target_link_libraries(
  precompiles
  runtime
)
