// arena.h - provides an arena for allocating memory.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>

#include <stack>

#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/perthread.h"
#include "hogd/base/types.h"
#include "hogd/container/merkle/rw_lock.h"

namespace hogd {

// An arena. This has a very simple implementation right now and needs more
// work. For example, we currently do not free any memory that we allocate.
// TODO(jhumphri): Make the arena production ready. Specifically, we cannot
// allow the global stack to grow unbounded--we need to free pointers back to
// the memory subsystem at some point.
//
// Example:
// Arena<Node> arena;
//
// Node *ptr = arena.alloc();
// CHECK_PTR(ptr);
//
// arena.free(ptr);
template<typename T> class Arena {
public:
  // The number of objects we allocate when we call malloc. By allocating a
  // batch of objects, we amortize the malloc overhead.
  static constexpr size_t kBatchSize = 512;

  // Creates an arena that allocates memory for T objects.
  Arena() : batch_alloc_size_(kBatchSize * sizeof(T))
  {
    CHECK_EQ(batch_alloc_size_ % sizeof(T), 0UL);
    CHECK_GT(batch_alloc_size_, 0UL);
  }

  // Disable copy and move.
  Arena(const Arena &) = delete;
  Arena &operator=(const Arena &) = delete;
  Arena(Arena &&) noexcept = delete;
  Arena &operator=(Arena &&) noexcept = delete;

  // Allocates a new memory region.
  T *alloc()
  {
    if (ptrs_->empty()) {
      grab_ptrs_from_global_stack();
    }
    // [ptrs_] can still be empty if [global_ptrs_] is empty.
    if (ptrs_->empty()) {
      // When we need to malloc more memory, we malloc enough memory to hold a
      // batch of objects rather than just a single object. This amortizes the
      // malloc overhead.
      char *batch =
          static_cast<char *>(aligned_alloc(kCacheLineSize, batch_alloc_size_));
      CHECK_PTR(batch);
      for (size_t i = 0; i < batch_alloc_size_; i += sizeof(T)) {
        ptrs_->push(reinterpret_cast<T *>(batch + i));
      }
    }
    T *ret = new (ptrs_->top()) T;
    ptrs_->pop();
    return ret;
  }

  // Frees a memory region. [ptr] must have been returne by [alloc()]
  // previously. The behavior is undefined if not.
  void free(T *ptr)
  {
    CHECK_PTR(ptr);

    ptr->~T();
    ptrs_->push(ptr);

    if (ptrs_->size() >= kBatchSizeToSpillToGlobalStack) {
      spill_ptrs_to_global_stack();
    }
  }

  size_t local_stack_size() const { return ptrs_->size(); }

  size_t global_stack_size()
  {
    global_lock_.lock_writer();
    size_t ret = global_ptrs_.size();
    global_lock_.unlock();
    return ret;
  }

private:
  static constexpr size_t kCacheLineSize = 64;
  // Once a local stack grows to this size on a call to [free()], it spills
  // pointers to the global stack.
  static constexpr size_t kBatchSizeToSpillToGlobalStack = kBatchSize * 2;
  // The size of the batch the arena allocates with malloc.
  const size_t batch_alloc_size_;

  // Tries to grab pointers from the global stack.
  void grab_ptrs_from_global_stack()
  {
    if (!global_ptrs_empty_.load(std::memory_order_relaxed)) {
      global_lock_.lock_writer();
      for (size_t i = 0; i < kBatchSize && !global_ptrs_.empty(); i++) {
        ptrs_->push(global_ptrs_.top());
        global_ptrs_.pop();
      }
      if (global_ptrs_.empty() &&
          !global_ptrs_empty_.load(std::memory_order_relaxed)) {
        global_ptrs_empty_.store(true, std::memory_order_relaxed);
      }
      global_lock_.unlock();
    }
  }

  // Spills an excess of pointers to the global stack.
  void spill_ptrs_to_global_stack()
  {
    global_lock_.lock_writer();
    while (ptrs_->size() > kBatchSize) {
      global_ptrs_.push(ptrs_->top());
      ptrs_->pop();
    }
    if (!global_ptrs_.empty() &&
        global_ptrs_empty_.load(std::memory_order_relaxed)) {
      global_ptrs_empty_.store(false, std::memory_order_relaxed);
    }
    global_lock_.unlock();
  }

  // Tracks the objects allocated by the arena.
  PerThread<std::stack<T *>> ptrs_;
  // A global stack shared by all threads.
  std::stack<T *> global_ptrs_;
  RwLock global_lock_;
  std::atomic<bool> global_ptrs_empty_ = true;
};

} // namespace hogd
