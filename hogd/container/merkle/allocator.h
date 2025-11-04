#pragma once

#include <optional>
#include <sys/mman.h>
#include <vector>

#include "hogd/base/log.h"

namespace hogd {

template<typename T> class BumpAllocator {
public:
  static constexpr size_t kNumMmapPages = 16;
  static constexpr size_t kMmapLength = kNumMmapPages * kPageSize;
  static_assert(sizeof(T) < kMmapLength);

  ~BumpAllocator() { free_all_regions(); }

  T *alloc()
  {
    if (!current_region_offset_ ||
        current_region_offset_ + sizeof(T) >
            allocated_regions_.back() + kMmapLength) {
      alloc_new_region();
    }
    CHECK_PTR(current_region_offset_);

    char *ret = current_region_offset_;
    current_region_offset_ += sizeof(T);
    return reinterpret_cast<T *>(ret);
  }

private:
  void alloc_new_region()
  {
    void *addr =
        mmap(/*addr=*/nullptr, kMmapLength, /*prot=*/PROT_READ | PROT_WRITE,
             /*flags=*/MAP_PRIVATE | MAP_ANONYMOUS, /*fildes=*/-1, /*off=*/0);
    CHECK_NE(addr, MAP_FAILED);
    CHECK_EQ(reinterpret_cast<uintptr_t>(addr) % kPageSize, 0UL);

    current_region_offset_ = static_cast<char *>(addr);
    allocated_regions_.push_back(static_cast<char *>(addr));
  }

  void free_all_regions()
  {
    for (char *region : allocated_regions_) {
      CHECK_EQ(munmap(region, kMmapLength), 0);
    }
  }

  char *current_region_offset_ = nullptr;
  std::vector<char *> allocated_regions_;
};

template<typename T> class Allocator {
public:
  enum Type {
    kDefault,
    kBump,
  };

  Allocator(Type type) : type_(type)
  {
    switch (type_) {
    case Type::kBump:
      bump_allocator_ = BumpAllocator<T>();
      break;
    default:
      CHECK(false);
      break;
    }
  }

  T *alloc()
  {
    switch (type_) {
    case Type::kDefault:
      return new T;
      break;
    case Type::kBump:
      CHECK(bump_allocator_.has_value());
      return bump_allocator_->alloc();
      break;
    default:
      CHECK(false);
      break;
    }
    return nullptr;
  }

  void free(T *t)
  {
    CHECK_PTR(t);

    switch (type_) {
    case Type::kDefault:
      delete t;
      break;
    case Type::kBump:
      CHECK(bump_allocator_.has_value());
      // We only free bump allocator objects when the bump allocator is
      // destructed.
      break;
    default:
      CHECK(false);
      break;
    }
  }

private:
  const Type type_;

  std::optional<BumpAllocator<T>> bump_allocator_;
};

} // namespace hogd
