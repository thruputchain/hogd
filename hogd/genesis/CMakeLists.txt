add_library(genesis
  genesis_configuration.cc
)

add_dependencies(
  genesis
  all_contract_headers
  generate_syscall_inner_h
  generate_syscall_meta_h
)

target_link_libraries(genesis
  base
  crypto
  storage_item
  protocol
  contract_loader
  components
)

target_include_directories(genesis PRIVATE
  ${CMAKE_BINARY_DIR}
  ${CMAKE_BINARY_DIR}/hogd/xdr
)
