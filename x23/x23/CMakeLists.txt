add_library(x23
  ${CMAKE_CURRENT_BINARY_DIR}/config.h
  async.h
  callsite.h
  cleanup.h
  corosync.cc
  corosync.h
  corotrace.h
  cpumap.cc
  cpumap.h
  err.h
  event.cc
  event.h
  event_impl.cc
  fastclock.h
  fileio.cc
  fileio.h
  format.h
  heap.h
  mvar.h
  refcnt.h
  rpc.cc
  rpc_common.cc
  rpc_common.h
  rpc.h
  rpc_msg.x
  serialize.cc
  serialize.h
  sockio.cc
  sockio.h
  srpc.cc
  srpc.h
  testlock.h
  threadpool.cc
  threadpool.h
  typename.h
  uiobuf.h
  utility.h
  vec.h
  x23.h
)
# Mark the implementation files as headers so they're included in IDE projects
# and distribution but not compiled separately
set_source_files_properties(
    event_impl_poll.cc
    event_impl_epoll.cc
    PROPERTIES HEADER_FILE_ONLY TRUE
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
