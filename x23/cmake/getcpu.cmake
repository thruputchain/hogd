include(CheckCXXSourceCompiles)

set(_CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})
string(REPLACE "-Werror" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

check_cxx_source_compiles([=[
#include <sched.h>

int
main()
{
  [[maybe_unused]] int fd{sched_getcpu()};
  return 0;
}
]=] HAVE_SCHED_GETCPU)

set(CMAKE_CXX_FLAGS ${_CMAKE_CXX_FLAGS})
