include(CheckCXXSourceCompiles)

check_cxx_source_compiles([=[
#include <atomic>
#include <memory>

int
main()
{
  struct std::atomic<std::shared_ptr<int>> aptr{nullptr};
  std::shared_ptr<int> ptr = aptr.load();
  return 0;
}
]=] HAVE_ATOMIC_SHARED_PTR)
