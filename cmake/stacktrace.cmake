include(CheckCXXSourceCompiles)

set(_CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++ -fexperimental-library")
endif()

check_cxx_source_compiles([=[
#include <stacktrace>

int
main()
{
  static_assert(__cpp_lib_stacktrace >= 202011L, "missing stacktrace");
}
]=] HAVE_CXX_STACKTRACE)

set(CMAKE_CXX_FLAGS "${_CMAKE_CXX_FLAGS}")

