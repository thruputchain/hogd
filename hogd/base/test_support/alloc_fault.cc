#include "hogd/base/test_support/alloc_fault.h"

#include <cstddef>
#include <cstdlib>
#include <new>

namespace {

thread_local int t_allocation_failure_countdown = -1;

#if !HOGD_SANITIZER_OWNS_OPERATOR_NEW
bool
should_fail_allocation() noexcept
{
  if (t_allocation_failure_countdown < 0) {
    return false;
  }
  if (t_allocation_failure_countdown == 0) {
    t_allocation_failure_countdown = -1;
    return true;
  }
  t_allocation_failure_countdown--;
  return false;
}
#endif

} // namespace

namespace hogd::test {

ScopedAllocationFailure::ScopedAllocationFailure(int allocations_before_failure)
{
  t_allocation_failure_countdown = allocations_before_failure;
}

ScopedAllocationFailure::~ScopedAllocationFailure()
{
  t_allocation_failure_countdown = -1;
}

} // namespace hogd::test

#if !HOGD_SANITIZER_OWNS_OPERATOR_NEW

void *
operator new(std::size_t size)
{
  if (should_fail_allocation()) {
    throw std::bad_alloc();
  }
  if (void *ptr = std::malloc(size == 0 ? 1 : size)) {
    return ptr;
  }
  throw std::bad_alloc();
}

void *
operator new[](std::size_t size)
{
  return ::operator new(size);
}

// Over-aligned types allocate through these, not the plain overloads.
void *
operator new(std::size_t size, std::align_val_t alignment)
{
  if (should_fail_allocation()) {
    throw std::bad_alloc();
  }
  const auto align = static_cast<std::size_t>(alignment);
  const std::size_t rounded = (size + align - 1) / align * align;
  if (void *ptr = std::aligned_alloc(align, rounded == 0 ? align : rounded)) {
    return ptr;
  }
  throw std::bad_alloc();
}

void *
operator new[](std::size_t size, std::align_val_t alignment)
{
  return ::operator new(size, alignment);
}

void
operator delete(void *ptr) noexcept
{
  std::free(ptr);
}

void
operator delete(void *ptr, std::size_t /*size*/) noexcept
{
  std::free(ptr);
}

void
operator delete[](void *ptr) noexcept
{
  std::free(ptr);
}

void
operator delete[](void *ptr, std::size_t /*size*/) noexcept
{
  std::free(ptr);
}

void
operator delete(void *ptr, std::align_val_t /*alignment*/) noexcept
{
  std::free(ptr);
}

void
operator delete(void *ptr, std::size_t /*size*/,
                std::align_val_t /*alignment*/) noexcept
{
  std::free(ptr);
}

void
operator delete[](void *ptr, std::align_val_t /*alignment*/) noexcept
{
  std::free(ptr);
}

void
operator delete[](void *ptr, std::size_t /*size*/,
                  std::align_val_t /*alignment*/) noexcept
{
  std::free(ptr);
}

#endif // !HOGD_SANITIZER_OWNS_OPERATOR_NEW
