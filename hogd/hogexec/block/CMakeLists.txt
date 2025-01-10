# create block module library
add_library(
  block
  block_frame.cc
)

target_link_libraries(block protocol)
