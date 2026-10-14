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

check_cxx_source_compiles([=[
#include <sched.h>
#include <unistd.h>

int
main()
{
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(0, &set);
  sched_setaffinity(0, sizeof(set), &set);
  return 0;
}
]=] HAVE_SCHED_SETAFFINITY)

set(CMAKE_CXX_FLAGS ${_CMAKE_CXX_FLAGS})
