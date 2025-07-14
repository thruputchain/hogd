#include <cstddef>
#include <cstring>
#include <future>
#include <gtest/gtest.h>
#include <memory>

#include <vector>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/container/merkle/arena.h"
#include "hogd/container/merkle/node.h"

namespace hogd {

TEST(Arena, Create)
{
  Arena<Node> arena;
  EXPECT_TRUE(arena.alloc());
}

// Basic test of global stack pointer spill.
TEST(Arena, GlobalStackBasicSpill)
{
  Arena<Node> arena;
  EXPECT_EQ(arena.local_stack_size(), 0);
  EXPECT_EQ(arena.global_stack_size(), 0);

  constexpr size_t kNumAllocs = 10'000;
  std::vector<Node *> ptrs;
  for (size_t i = 0; i < kNumAllocs; i++) {
    ptrs.push_back(arena.alloc());
    EXPECT_TRUE(ptrs.back());
  }
  EXPECT_EQ(arena.local_stack_size(),
            Arena<Node>::kBatchSize - (kNumAllocs % Arena<Node>::kBatchSize));
  EXPECT_EQ(arena.global_stack_size(), 0);
  for (size_t i = 0; i < kNumAllocs; i++) {
    arena.free(ptrs[i]);
  }
  EXPECT_EQ(arena.local_stack_size(), Arena<Node>::kBatchSize);
  EXPECT_EQ(arena.global_stack_size(),
            kNumAllocs - (kNumAllocs % Arena<Node>::kBatchSize));
}

// Tests that we can spill and grab pointers from the global stack.
TEST(Arena, GlobalStackSpillAndGrab)
{
  Arena<Node> arena;
  EXPECT_EQ(arena.local_stack_size(), 0);
  EXPECT_EQ(arena.global_stack_size(), 0);

  constexpr size_t kNumAllocs = 10'000;
  constexpr size_t kNumFrees = Arena<Node>::kBatchSize * 2;
  std::vector<Node *> ptrs;
  // Allocate the pointers.
  for (size_t i = 0; i < kNumAllocs; i++) {
    ptrs.push_back(arena.alloc());
    EXPECT_TRUE(ptrs.back());
  }
  constexpr size_t kExpectedLocalStackSizeAfterAlloc =
      Arena<Node>::kBatchSize - (kNumAllocs % Arena<Node>::kBatchSize);
  EXPECT_EQ(arena.local_stack_size(), kExpectedLocalStackSizeAfterAlloc);
  EXPECT_EQ(arena.global_stack_size(), 0);
  // Free the pointers.
  for (size_t i = 0; i < kNumFrees; i++) {
    arena.free(ptrs[i]);
  }
  constexpr size_t kExpectedLocalStackSizeAfterFree =
      kExpectedLocalStackSizeAfterAlloc + Arena<Node>::kBatchSize;
  EXPECT_EQ(arena.local_stack_size(), kExpectedLocalStackSizeAfterFree);
  EXPECT_EQ(arena.global_stack_size(), Arena<Node>::kBatchSize);

  // Allocate the remaining pointers in the local stack.
  for (size_t i = 0; i < kExpectedLocalStackSizeAfterFree; i++) {
    ptrs.push_back(arena.alloc());
    EXPECT_TRUE(ptrs.back());
  }
  EXPECT_EQ(arena.local_stack_size(), 0);
  EXPECT_EQ(arena.global_stack_size(), Arena<Node>::kBatchSize);

  // Now allocate one more pointer. We expect the arena to allocate another
  // batch on the local stack, so after the pointer allocation, the local stack
  // size should be [Arena<Node>::kBatchSize - 1].
  ptrs.push_back(arena.alloc());
  EXPECT_TRUE(ptrs.back());
  EXPECT_EQ(arena.local_stack_size(), Arena<Node>::kBatchSize - 1);
  EXPECT_EQ(arena.global_stack_size(), 0);
}

// Tests that multiple threads can spill and grab pointers from the global
// stack.
TEST(Arena, Multithread)
{
  constexpr size_t kNumThreads = 32;
  constexpr size_t kNumIterations = 50;
  constexpr size_t kNumAllocsPerIteration = 1024;

  WorkerPool pool(kNumThreads);
  Arena<Node> arena;
  std::vector<std::future<void>> futures;
  futures.reserve(kNumThreads);
  for (size_t i = 0; i < kNumThreads; i++) {
    futures.push_back(pool.submit([&arena] {
      std::vector<Node *> ptrs;
      for (size_t i = 0; i < kNumIterations; i++) {
        for (size_t j = 0; j < kNumAllocsPerIteration; j++) {
          ptrs.push_back(arena.alloc());
          EXPECT_TRUE(ptrs.back());
        }
        for (size_t j = 0; j < kNumAllocsPerIteration; j++) {
          arena.free(ptrs[j]);
        }
        EXPECT_LE(arena.global_stack_size(),
                  kNumThreads * Arena<Node>::kBatchSize);
        ptrs.clear();
      }
      EXPECT_EQ(arena.local_stack_size(), Arena<Node>::kBatchSize);
    }));
  }
  for (std::future<void> &future : futures) {
    future.get();
  }
  EXPECT_LE(arena.global_stack_size(), kNumThreads * Arena<Node>::kBatchSize);
}

} // namespace hogd

int
main(int argc, char *argv[])
{
  testing::InitGoogleTest(&argc, argv);
  // Initialize logging. This is needed for the CHECK macros.
  log_init(std::make_unique<hogd::ConsoleLogger>());

  return RUN_ALL_TESTS();
}
