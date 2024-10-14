include(CheckCXXSourceCompiles)

check_cxx_source_compiles([=[
#include <cstddef>
#include <new>

int
main()
{
  std::size_t size = std::hardware_destructive_interference_size;
  return 0;
}
]=] HAVE_HARDWARE_DESTRUCTIVE_INTERFERENCE_SIZE)
