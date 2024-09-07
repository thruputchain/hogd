add_subdirectory(base)
add_subdirectory(crypto)
add_subdirectory(syscall)

add_executable(hello
  hello.cc
)

add_dependencies(hello libsodium)

target_link_libraries(hello
  base
  ${LIBSODIUM_LIBRARIES}
)
