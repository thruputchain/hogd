include(CheckCXXSourceCompiles)

check_cxx_source_compiles([=[
#include <stacktrace>

int
main()
{
  auto st = std::stacktrace::current();
  return 0;
}
]=] HAVE_CXX_STACKTRACE)
