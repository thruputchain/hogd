#include <array>
#include <cstddef>
#include <cstring>
#include <gtest/gtest.h>
#include <memory>
#include <optional>

#include <string>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/container/merkle/arena.h"
#include "hogd/container/merkle/hex_array.h"
#include "hogd/container/merkle/node.h"
#include "types.hh"

namespace hogd {

TEST(Arena, Create)
{
  Arena<Node> arena;
  EXPECT_TRUE(arena.alloc());
}

TEST(Arena, GlobalStackBasic)
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
  EXPECT_EQ(arena.local_stack_size(), 240);
  EXPECT_EQ(arena.global_stack_size(), 0);
  for (size_t i = 0; i < kNumAllocs; i++) {
    arena.free(ptrs[i]);
  }
  EXPECT_EQ(arena.local_stack_size(), 512);
  EXPECT_EQ(arena.global_stack_size(), 9728);
}

TEST(Arena, GlobalStackSpillAndGrab)
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
  EXPECT_EQ(arena.local_stack_size(), 240);
  EXPECT_EQ(arena.global_stack_size(), 0);
  for (size_t i = 0; i < 1024; i++) {
    arena.free(ptrs[i]);
  }
  EXPECT_EQ(arena.local_stack_size(), 752);
  EXPECT_EQ(arena.global_stack_size(), 512);

  for (size_t i = 0; i < 752; i++) {
    ptrs.push_back(arena.alloc());
    EXPECT_TRUE(ptrs.back());
  }
  EXPECT_EQ(arena.local_stack_size(), 0);
  EXPECT_EQ(arena.global_stack_size(), 512);

  ptrs.push_back(arena.alloc());
  EXPECT_TRUE(ptrs.back());
  EXPECT_EQ(arena.local_stack_size(), 511);
  EXPECT_EQ(arena.global_stack_size(), 0);
}

TEST(Arena, Multithread)
{
  constexpr size_t kNumThreads = 32;
  WorkerPool pool(kNumThreads);
  Arena<Node> arena;
  std::vector<std::future<void>> futures;
  for (size_t i = 0; i < kNumThreads; i++) {
    futures.push_back(pool.submit([&arena] {
      std::vector<Node *> ptrs;
      for (size_t i = 0; i < 100; i++) {
        for (size_t j = 0; j < 1024; j++) {
          ptrs.push_back(arena.alloc());
          EXPECT_TRUE(ptrs.back());
        }
        for (size_t j = 0; j < 1024; j++) {
          arena.free(ptrs[j]);
        }
        EXPECT_LE(arena.global_stack_size(), kNumThreads * 512);
        ptrs.clear();
      }
      EXPECT_EQ(arena.local_stack_size(), 512);
    }));
  }
  for (std::future<void> &future : futures) {
    future.get();
  }
  EXPECT_LE(arena.global_stack_size(), kNumThreads * 512);
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
