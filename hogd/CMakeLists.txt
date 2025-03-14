# Build each shared module
add_subdirectory(crypto)
add_subdirectory(base)
add_subdirectory(syscall_spec)
add_subdirectory(xdr)
add_subdirectory(shared_util)

# Then combine them into the hogd library (used by all executables below)
add_library(
  hogd
  init.cc
)
target_link_libraries(
  hogd
  base
  crypto
  shared_util
)

# Then build the modules for the hogdb and hogexec executables
add_subdirectory(coord)
add_subdirectory(hogdb)
add_subdirectory(hogexec)
add_subdirectory(rs_contracts)
add_subdirectory(test)
add_subdirectory(loadgen)

add_subdirectory(test_ffi)
