include(CheckCXXSourceCompiles)

check_cxx_source_compiles([=[
#include <sys/epoll.h>

int
main()
{
  int fd = epoll_create1(0);
  return 0;
}
]=] HAVE_EPOLL)
