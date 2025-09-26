include(CheckCXXSourceCompiles)

set(_CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})
string(REPLACE "-Werror" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

check_cxx_source_compiles([=[
#include <stdlib.h>

int
main()
{
  const char *p = getprogname();
}
]=] HAVE_GETPROGNAME)

set(CMAKE_CXX_FLAGS ${_CMAKE_CXX_FLAGS})
