# Build each shared module
add_subdirectory(base)
add_subdirectory(crypto)
add_subdirectory(syscall_spec)
add_subdirectory(xdr)

# Then combine them into the hogd library (used by all executables below)
add_library(
  hogd
  init.cc
)
target_link_libraries(
  hogd
  base
  crypto
)

# Then build the modules for the hogdb and hogexec executables
add_subdirectory(coord)
add_subdirectory(hogdb)
add_subdirectory(hogexec)
