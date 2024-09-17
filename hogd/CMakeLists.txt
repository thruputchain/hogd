add_subdirectory(base)
add_subdirectory(crypto)
add_subdirectory(runtime_trampoline)
add_subdirectory(wasmi)
add_subdirectory(runtime)


add_executable(hello
  hello.cc
)

add_dependencies(hello libsodium)

target_link_libraries(hello
  base
  ${LIBSODIUM_LIBRARIES}
)
