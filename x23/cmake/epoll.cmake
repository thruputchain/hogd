# Add the option first - defaults to ON
option(EPOLL "Enable epoll support if available" ON)

include(CheckCXXSourceCompiles)

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
