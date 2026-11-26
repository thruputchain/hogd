# PointerTagSanitizer shims for the libraries hogd links that the PTSan
# compiler did not build; shim.h describes the boundary. The glibc shims are
# set up in cmake/ptsan.cmake, because every executable needs them.

hogd_ptsan_shim_library(ptsan_rust_shims rust_shims.cc)
target_link_libraries(ptsan_rust_shims PUBLIC unified_lib)
target_include_directories(ptsan_rust_shims PRIVATE ${CMAKE_BINARY_DIR})
add_dependencies(ptsan_rust_shims generate_crypto_ffi_h)
set_property(TARGET unified_lib APPEND PROPERTY
  INTERFACE_LINK_LIBRARIES ptsan_rust_shims)

# OpenSSL::Crypto is the top-level imported target from cmake/ptsan.cmake.
hogd_ptsan_shim_library(ptsan_openssl_shims openssl_shims.cc)
target_link_libraries(ptsan_openssl_shims PUBLIC OpenSSL::Crypto)
set_property(TARGET OpenSSL::Crypto APPEND PROPERTY
  INTERFACE_LINK_LIBRARIES ptsan_openssl_shims)
