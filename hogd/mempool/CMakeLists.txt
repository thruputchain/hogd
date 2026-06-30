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
  http
  hogdb
  merkle
  metrics
  protocol
  runtime
)

add_executable(
  mempool_exec
  mempool.cc
)
target_link_libraries(
  mempool_exec
  mempool
)

if(CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
  set_property(TARGET mempool_exec PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()
