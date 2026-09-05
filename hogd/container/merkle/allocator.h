#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <new>
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

  BumpAllocator()
  {
    rw_lock_.lock_writer();
    alloc_new_region();
    rw_lock_.unlock();
  }

  ~BumpAllocator() { free_all_regions(); }

  T *alloc()
  {
    PrivateRegion &region = *private_region_;
    if (region.num_elements_ == 0) {
      region.region_ = alloc_from_pool();
      CHECK_PTR(region.region_);
      region.num_elements_ = PerThreadNumElementsPerChunk;
    }

    region.num_elements_--;
    T *ret = reinterpret_cast<T *>(region.region_);
    region.region_ += sizeof(T);
    return ret;
  }

  void reset_all_regions()
  {
    std::unique_lock ul = private_region_.lock_threads();
    private_region_.for_each(ul, [](PrivateRegion &private_region) {
      private_region.region_ = nullptr;
      private_region.num_elements_ = 0;
    });

    rw_lock_.lock_writer();
    CHECK(!allocated_regions_.empty());
    CHECK_GE(current_region_idx_, 0L);
    current_region_idx_ = 0;
    current_region_offset_.store(allocated_regions_[0],
                                 std::memory_order_relaxed);
    rw_lock_.unlock();
  }

private:
  struct PrivateRegion {
    unsigned char *region_{nullptr};
    size_t num_elements_{0};
  };

  unsigned char *alloc_from_pool()
  {
    while (true) {
      rw_lock_.lock_reader();

      unsigned char *current_region_offset =
          current_region_offset_.load(std::memory_order_relaxed);
      CHECK_PTR(current_region_offset);
      CHECK_LT(current_region_idx_,
               static_cast<ssize_t>(allocated_regions_.size()));
      // Avoid forming a pointer past the mmap region (UB if beyond
      // one-past-the-end)
      auto remaining = static_cast<size_t>(
          (allocated_regions_[current_region_idx_] + kMmapLength) -
          current_region_offset);
      if (kPerThreadAllocLength > remaining) {

        // We may need to allocate a new region. Only one thread should should
        // do this while the others wait.
        if (rw_lock_.try_upgrade_to_writer_if_only_one()) {
          // This thread won the race and will allocate the new region.
          if (current_region_idx_ ==
              static_cast<ssize_t>(allocated_regions_.size()) - 1) {
            alloc_new_region();
          }
          else {
            // If we reset the bump allocator with [reset_all_regions()], then
            // the bump allocator will have extra memory regions already mmap'd.
            // Thus, we reuse these extra memory regions before allocating more
            // with [mmap()].
            current_region_idx_++;
            current_region_offset_.store(
                allocated_regions_[current_region_idx_],
                std::memory_order_relaxed);
          }
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
    // POSIX guarantees that a successful anonymous mmap() returns a
    // page-aligned address. On the LP64 platforms we support (Linux x86-64 and
    // ARM64 under GCC/Clang), reinterpret_cast<uintptr_t> yields the numeric
    // virtual address, so this is a defensive sanity check on that guarantee
    // rather than a portability hazard.
    CHECK_EQ(reinterpret_cast<uintptr_t>(addr) % kPageSize, 0UL);

#ifdef __cpp_lib_start_lifetime_as
    unsigned char *region =
        std::start_lifetime_as_array<unsigned char>(addr, kMmapLength);
#else
    struct MmapRegion {
      unsigned char data[kMmapLength];
    };
    static_assert(sizeof(MmapRegion) == kMmapLength);
    unsigned char *region = (::new (addr) MmapRegion)->data;
#endif

    current_region_offset_.store(region, std::memory_order_relaxed);
    allocated_regions_.push_back(region);
    current_region_idx_++;
  }

  void free_all_regions()
  {
    for (unsigned char *region : allocated_regions_) {
      CHECK_EQ(munmap(region, kMmapLength), 0);
    }
  }

  RwLock rw_lock_;
  std::atomic<unsigned char *> current_region_offset_ = nullptr;
  ssize_t current_region_idx_ = -1;
  std::vector<unsigned char *> allocated_regions_;
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

  void clear_all_regions()
  {
    switch (type_) {
    case Type::kBump:
      CHECK(bump_allocator_.has_value());
      // We support clearing (but not freeing) regions for the bump allocator
      // only.
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      bump_allocator_->reset_all_regions();
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
