include(CheckCXXSourceCompiles)

check_cxx_source_compiles([=[
#include <sys/socket.h>

int
main()
{
  int fd = accept4(0, nullptr, nullptr, SOCK_NONBLOCK|SOCK_CLOEXEC);
  return 0;
}
]=] HAVE_ACCEPT4)
