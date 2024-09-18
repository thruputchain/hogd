include(CheckCXXSourceCompiles)

list(APPEND CMAKE_REQUIRED_LINK_OPTIONS "-lstdc++exp")

check_cxx_source_compiles([=[
#include <stacktrace>

int
main()
{
  auto st = std::stacktrace::current();
  return 0;
}
]=] HAVE_CXX_STACKTRACE)
