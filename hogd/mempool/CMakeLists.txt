add_subdirectory(test)

add_library(
  mempool
  rpc_from_coord.cc
  scheduler.cc
)
target_link_libraries(
  mempool
  base
  crypto
  exec_interface
  hogd
  hogdb
  merkle
  protocol
  runtime
  auth_claim
)
set_property(TARGET mempool PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)

add_executable(
  mempool_exec
  mempool.cc
)
target_link_libraries(
  mempool_exec
  mempool
)
