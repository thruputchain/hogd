# Add the option first - defaults to ON
option(EPOLL "Enable epoll support if available" ON)

include(CheckCXXSourceCompiles)

set(_CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})
string(REPLACE "-Werror" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

# Only check for epoll if USE_EPOLL is enabled
if(EPOLL)
    check_cxx_source_compiles([=[
    #include <sys/epoll.h>
    
    int
    main()
    {
      int fd = epoll_create1(0);
      return 0;
    }
    ]=] USE_EPOLL)
else()
    set(USE_EPOLL 0)
endif()

set(CMAKE_CXX_FLAGS ${_CMAKE_CXX_FLAGS})
