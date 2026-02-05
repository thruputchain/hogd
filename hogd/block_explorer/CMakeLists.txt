# add_library(block_explorer_lib STATIC
# )
#
# target_link_libraries(block_explorer_lib PUBLIC
#     hogd
# )

add_executable(
  block_explorer
  block_explorer.cc
)
target_link_libraries(
  block_explorer
  hogd
  # block_explorer_lib
)
