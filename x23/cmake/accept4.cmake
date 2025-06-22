include(CheckCXXSourceCompiles)

set(_CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})
string(REPLACE "-Werror" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

check_cxx_source_compiles([=[
#include <sys/socket.h>

int
main()
{
  int fd = accept4(0, nullptr, nullptr, SOCK_NONBLOCK|SOCK_CLOEXEC);
  return 0;
}
]=] HAVE_ACCEPT4)

set(CMAKE_CXX_FLAGS ${_CMAKE_CXX_FLAGS})
