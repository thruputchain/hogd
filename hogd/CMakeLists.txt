# Build each module
add_subdirectory(base)
add_subdirectory(crypto)
add_subdirectory(runtime_trampoline)
add_subdirectory(wasmi)
add_subdirectory(runtime)
add_subdirectory(xdr)

# Build the main hogd library
add_library(
  hogd
  init.cc
)
target_link_libraries(
  hogd
  base
  crypto
  ${LIBSODIUM_LIBRARIES}
)
add_dependencies(hogd libsodium)

# Build the backend database (hogdb) executable
add_executable(
  hogdb
  hogdb.cc
)
target_link_libraries(
  hogdb
  hogd
)

# Build the frontend executor (hogexec) executable
add_executable(
  hogexec
  hogexec.cc
)
target_link_libraries(
  hogexec
  hogd
)
