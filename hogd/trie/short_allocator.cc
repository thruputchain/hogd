#include "hogd/trie/short_allocator.h"

#include <cstdint>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>

namespace hog::detail {

void
AllocationVerifier::check_get(uint32_t ptr)
{

  std::lock_guard lock(mtx_);
  if ((ptr != 0) && (!allocs_.contains(ptr))) {
    throw std::runtime_error("invalid get!");
  }
}

void
AllocationVerifier::check_allocate(uint32_t ptr)
{

  std::lock_guard lock(mtx_);
  if (allocs_.contains(ptr)) {
    throw std::runtime_error("invalid alloc of ptr " + std::to_string(ptr));
  }
  allocs_.insert(ptr);
}

void
AllocationVerifier::check_free(uint32_t ptr)
{
  std::lock_guard lock(mtx_);
  if (!allocs_.contains(ptr)) {
    throw std::runtime_error("invalid free on " + std::to_string(ptr));
  }
  allocs_.erase(ptr);
}

void
AllocationVerifier::assert_empty() const noexcept
{
  if (!allocs_.empty()) {
    // NOLINTNEXTLINE
    std::printf("potential UAF: failed to clean up AllocationVerifier\n");
    std::terminate();
  }
}

AllocationVerifier::~AllocationVerifier()
{
  assert_empty();
}

} // namespace hog::detail
