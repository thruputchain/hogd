add_subdirectory(base)

add_executable(hello
  hello.cc
)

target_compile_options(hello
  PRIVATE ${LIBSODIUM_CFLAGS_OTHER}
)

target_include_directories(hello
  PRIVATE ${LIBSODIUM_INCLUDE_DIRS}
)

target_link_libraries(hello
  base
  ${LIBSODIUM_LIBRARIES}
)
