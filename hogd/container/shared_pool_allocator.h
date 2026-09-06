#pragma once

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <utility>

namespace hogd {

// A shard has one pool, not one pool per allocating thread. Transactions are
// commonly freed by a different worker; keeping a single free list per shard
// avoids synchronized_pool_resource's cross-thread pool lookup overhead.
class SharedPoolResource {
public:
  void *allocate(size_t bytes, size_t alignment)
  {
    std::lock_guard guard(mutex_);
    return pool_.allocate(bytes, alignment);
  }
  void deallocate(void *ptr, size_t bytes, size_t alignment)
  {
    std::lock_guard guard(mutex_);
    pool_.deallocate(ptr, bytes, alignment);
  }

private:
  std::mutex mutex_;
  std::pmr::unsynchronized_pool_resource pool_;
};

// Keep the resource alive even when an allocated shared_ptr outlives its owner.
template<typename T> class SharedPoolAllocator {
public:
  using value_type = T;

  explicit SharedPoolAllocator(std::shared_ptr<SharedPoolResource> resource)
    : resource_(std::move(resource))
  {}

  template<typename U>
  SharedPoolAllocator(const SharedPoolAllocator<U> &other)
    : resource_(other.resource_)
  {}

  T *allocate(size_t count)
  {
    if (count > static_cast<size_t>(-1) / sizeof(T)) {
      throw std::bad_array_new_length();
    }
    return static_cast<T *>(resource_->allocate(count * sizeof(T), alignof(T)));
  }
  void deallocate(T *ptr, size_t count)
  {
    resource_->deallocate(ptr, count * sizeof(T), alignof(T));
  }
  template<typename U>
  bool operator==(const SharedPoolAllocator<U> &other) const noexcept
  {
    return resource_ == other.resource_;
  }

private:
  template<typename> friend class SharedPoolAllocator;
  std::shared_ptr<SharedPoolResource> resource_;
};

} // namespace hogd
