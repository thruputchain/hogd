add_subdirectory(base)

add_executable(hello
  hello.cc
)

target_link_libraries(hello
  base
)
