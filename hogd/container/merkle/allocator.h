#pragma once

#include <atomic>
#include <optional>
#include <sys/mman.h>
#include <vector>

#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/perthread.h"
#include "hogd/base/types.h"
#include "hogd/container/merkle/rw_lock.h"

namespace hogd {

template<typename T, size_t PerThreadNumElementsPerChunk = 128>
class BumpAllocator {
public:
  static constexpr size_t kPerThreadAllocLength =
      PerThreadNumElementsPerChunk * sizeof(T);

  static constexpr size_t kNumMmapPages = 16'384;
  static constexpr size_t kMmapLength = kNumMmapPages * kPageSize;
  static_assert(kPerThreadAllocLength < kMmapLength);

  BumpAllocator() = default;

  ~BumpAllocator() { free_all_regions(); }

  T *alloc()
  {
    if (private_region_->num_elements_ == 0) {
      private_region_->region_ = alloc_from_pool();
      CHECK_PTR(private_region_->region_);
      private_region_->num_elements_ = PerThreadNumElementsPerChunk;
    }

    // The [--] operator (when placed after a variable) has higher precendence
    // than the [*] operator.
    private_region_->num_elements_--;
    T *ret = reinterpret_cast<T *>(private_region_->region_);
    private_region_->region_ += sizeof(T);
    return ret;
  }

private:
  struct PrivateRegion {
    char *region_{nullptr};
    size_t num_elements_{0};
  };

  char *alloc_from_pool()
  {
    while (true) {
      char *current_region_offset =
          current_region_offset_.load(std::memory_order_relaxed);

      rw_lock_.lock_reader();
      if (!current_region_offset ||
          current_region_offset + kPerThreadAllocLength >
              allocated_regions_.back() + kMmapLength) {

        // We may need to allocate a new region. Only one thread should should
        // do this while the others wait.
        if (rw_lock_.try_upgrade_to_writer_if_only_one()) {
          // This thread won the race and will allocate the new region.
          alloc_new_region();
          rw_lock_.unlock();
        }
        else {
          rw_lock_.unlock();
          // Another thread won the race and is allocating the new region. Wait
          // until that thread is done.
          while (current_region_offset_.load(std::memory_order_relaxed) ==
                 current_region_offset) {
            cpu_relax();
          }
        }
        continue;
      }
      rw_lock_.unlock();

      CHECK_PTR(current_region_offset);

      if (current_region_offset_.compare_exchange_weak(
              /*expected=*/current_region_offset,
              /*desired=*/current_region_offset + kPerThreadAllocLength,
              /*order=*/std::memory_order_relaxed)) {
        return current_region_offset;
      }
    }
  }

  void alloc_new_region()
  {
    CHECK(rw_lock_.is_writer_locked_relaxed());

    void *addr =
        mmap(/*addr=*/nullptr, kMmapLength, /*prot=*/PROT_READ | PROT_WRITE,
             /*flags=*/MAP_PRIVATE | MAP_ANONYMOUS, /*fildes=*/-1, /*off=*/0);
    CHECK_NE(addr, MAP_FAILED);
    CHECK_EQ(reinterpret_cast<uintptr_t>(addr) % kPageSize, 0UL);

    current_region_offset_.store(static_cast<char *>(addr),
                                 std::memory_order_relaxed);
    allocated_regions_.push_back(static_cast<char *>(addr));
  }

  void free_all_regions()
  {
    for (char *region : allocated_regions_) {
      CHECK_EQ(munmap(region, kMmapLength), 0);
    }
  }

  RwLock rw_lock_;
  std::atomic<char *> current_region_offset_ = nullptr;
  std::vector<char *> allocated_regions_;
  PerThread<PrivateRegion> private_region_;
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
    case Type::kDefault:
      break;
    case Type::kBump:
      bump_allocator_.emplace();
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
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
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

  Type type() const { return type_; }

private:
  const Type type_;

  std::optional<BumpAllocator<T>> bump_allocator_;
};

} // namespace hogd
