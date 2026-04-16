# Build each shared module
add_subdirectory(base)
add_subdirectory(container)
add_subdirectory(rust)
add_subdirectory(crypto)
add_subdirectory(event)
add_subdirectory(metrics)
add_subdirectory(mockdb)
add_subdirectory(db_interface)
add_subdirectory(storage_item)
add_subdirectory(storage_cost)
add_subdirectory(syscall_spec)
add_subdirectory(xdr)
add_subdirectory(genesis)
add_subdirectory(loadgen)
add_subdirectory(auth_claim)
add_subdirectory(auth)

# Then combine them into the hogd library (used by all executables below)
add_library(
  hogd
  init.cc
  version.c
)
add_library(hogd_interface INTERFACE)
target_link_libraries(
  hogd_interface INTERFACE
  built_headers_interface
  crypto_interface
  protocol
)

target_link_libraries(
  hogd
  base
  crypto
  hogd_interface
)

# Additional libraries that are not included by default (i.e., in hogd library)
add_subdirectory(ramdb)
add_subdirectory(indexer)
add_subdirectory(block_explorer)
# add_subdirectory(rpc)

# Finally, build the hogdb, hogexec, and mempool executables
add_subdirectory(hogdb)
add_subdirectory(hogexec)
add_subdirectory(mempool)

# Build utilities
add_subdirectory(utils)

# Build module for mock contract tests
if (ENABLE_TEST_FFI)
  add_subdirectory(test_ffi)
endif()

add_custom_target(tidy_targets)
add_dependencies(
  tidy_targets
  all_wasm_headers
  protocol
  x23
  hogd_interface
  components_library_exception
  generate_syscall_inner_h
  generate_syscall_meta_h
  generate_wasmi_ffi_h
  generate_wasmtime_ffi_h
  generate_contract_types_ffi_h
  generate_crypto_ffi_h
  generate_wasm_meter_ffi_h
  roundtrip_hh
  xdrtest
)
