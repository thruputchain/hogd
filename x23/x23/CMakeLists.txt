add_library(x23
  ${CMAKE_CURRENT_BINARY_DIR}/config.h
  async.h
  cleanup.h
  corosync.h
  corosync.cc
  corotrace.h
  corotrace.cc
  err.h
  event.cc
  event.h
  event_impl.cc
  fileio.cc
  fileio.h
  format.h
  growvec.h
  heap.h
  mvar.h
  refcnt.h
  rpc.cc
  rpc.h
  rpc_msg.x
  serialize.h
  sockio.h
  sockio.cc
  testlock.h
  threadpool.cc
  threadpool.h
  utility.h
  vec.h
  x23.h
)

use_x23(x23)

target_include_directories(x23
  PUBLIC
  $<BUILD_INTERFACE:${X23_SOURCE_DIR}>
  $<BUILD_INTERFACE:${X23_BINARY_DIR}>
  $<INSTALL_INTERFACE:include>
)

get_target_property(x23_sources x23 SOURCES)
set(x23_headers "")
foreach(source ${x23_sources})
  if(${source} MATCHES ".*\\.h$" OR ${source} MATCHES ".*\\.hh$")
    list(APPEND x23_headers ${source})
  endif()
endforeach()

# Set the public headers property
set_target_properties(x23 PROPERTIES PUBLIC_HEADER "${x23_headers}")

if(X23_IS_TOP_LEVEL)
  install(TARGETS x23
    EXPORT x23Targets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
    PUBLIC_HEADER DESTINATION include/x23
  )

  install(FILES x23_gdb.py
    DESTINATION "${CMAKE_INSTALL_PREFIX}/share/gdb/python"
  )
endif()
