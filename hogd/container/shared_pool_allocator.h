#pragma once

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <utility>

namespace hogd {

// Keep the resource alive even when an allocated shared_ptr outlives its owner.
template<typename T> class SharedPoolAllocator {
public:
  using value_type = T;

  explicit SharedPoolAllocator(
      std::shared_ptr<std::pmr::synchronized_pool_resource> resource)
    : resource_(std::move(resource))
  {}

  template<typename U>
  SharedPoolAllocator(const SharedPoolAllocator<U> &other)
    : resource_(other.resource_)
  {}

  T *allocate(size_t count)
  {
    return std::pmr::polymorphic_allocator<T>(resource_.get()).allocate(count);
  }
  void deallocate(T *ptr, size_t count)
  {
    std::pmr::polymorphic_allocator<T>(resource_.get()).deallocate(ptr, count);
  }
  template<typename U>
  bool operator==(const SharedPoolAllocator<U> &other) const noexcept
  {
    return resource_ == other.resource_;
  }

private:
  template<typename> friend class SharedPoolAllocator;
  std::shared_ptr<std::pmr::synchronized_pool_resource> resource_;
};

} // namespace hogd
