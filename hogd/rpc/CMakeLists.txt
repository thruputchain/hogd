# create rpc module library
add_library(
  rpc
  rpc.cc
)
target_link_libraries(
  rpc
  hogd
)

# Build the rpc_bench executable
add_executable(
  rpc_bench
  rpc_bench.cc
)
target_link_libraries(
  rpc_bench
  rpc
)
