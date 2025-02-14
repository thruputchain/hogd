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

# Additional libraries that are not included by default (i.e., in hogd library)
add_subdirectory(ramdb)

# Finally, build the hogdb, hogexec, and coord executables
add_subdirectory(hogdb)
add_subdirectory(hogexec)
add_subdirectory(coord)
