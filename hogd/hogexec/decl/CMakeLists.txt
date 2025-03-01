add_library(
  decl
  lficpp.cc
  sandbox_cache.cc
  signal_handler.cc
)

add_dependencies(decl liblfi)
target_link_libraries(
  decl
  liblfi_lib
  liblfiv_lib
  runtime_trampoline
)