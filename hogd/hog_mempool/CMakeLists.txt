add_subdirectory(test)

add_library(
  hog_mempool
  rpc_from_coord.cc
  scheduler.cc
)
target_link_libraries(
  hog_mempool
  base
  crypto
  exec_interface
  hogd
  http
  hog_db
  merkle
  metrics
  protocol
  runtime
)

add_executable(
  hog-mempool
  hog_mempool.cc
)
target_link_libraries(
  hog-mempool
  hog_mempool
)

if(CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
  set_property(TARGET hog-mempool PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()
