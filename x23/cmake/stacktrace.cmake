include(CheckCXXSourceCompiles)

check_cxx_source_compiles([=[
#include <format>
#include <stacktrace>

static_assert(__cpp_lib_formatters >= 202302L,
              "Need format support for stack traces");

int
main()
{
  [[maybe_unused]] std::string s =
      std::format("{}", std::stacktrace::current());
}
]=] HAVE_STD_STACKTRACE)

check_cxx_source_compiles([=[
#include <execinfo.h>
#include <stdlib.h>

int
main()
{
  void *bt[64];
  int n = backtrace(bt, 64);
  char **syms = backtrace_symbols(bt, n);
  free(syms);
}
]=] HAVE_BACKTRACE_SYMBOLS)
