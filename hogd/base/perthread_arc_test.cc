#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "hogd/base/perthread_arc.h"

namespace hogd {

TEST(PerThreadARCTest, BasicUsage)
{
  PerThreadARC<int> arc(1);

  EXPECT_TRUE(arc.try_acquire_reference());
  EXPECT_TRUE(arc.try_acquire_reference());
  EXPECT_FALSE(arc.release_reference());
  EXPECT_FALSE(arc.initiate_shutdown());
  EXPECT_TRUE(arc.release_reference());
}

TEST(PerThreadARCTest, NoReferences)
{
  PerThreadARC<int> arc;
  EXPECT_TRUE(arc.initiate_shutdown());
}

TEST(PerThreadARCTest, NeverModifiedObject)
{
  PerThreadARC<int> arc(1);
}

TEST(PerThreadARCTest, NotShutdownObject)
{
  PerThreadARC<int> arc(1);
  EXPECT_TRUE(arc.try_acquire_reference());
  EXPECT_FALSE(arc.release_reference());
}

TEST(PerThreadARCTest, TryAcquireAfterShutdown)
{
  PerThreadARC<int> arc{};
  EXPECT_TRUE(arc.try_acquire_reference());
  EXPECT_FALSE(arc.initiate_shutdown());
  EXPECT_FALSE(arc.try_acquire_reference());
  EXPECT_TRUE(arc.release_reference());
}

TEST(PerThreadARCTest, DifferentThreadsAcquireAndRelease)
{
  PerThreadARC<int> arc{};

  std::atomic<bool> done{false};
  std::atomic<bool> t1_done{false};
  std::atomic<bool> t2_done{false};

  std::jthread t1([&] {
    EXPECT_TRUE(arc.try_acquire_reference());
    EXPECT_TRUE(arc.try_acquire_reference());
    t1_done.store(true);
    t1_done.notify_all();
    done.wait(false);
  });

  std::jthread t2([&] {
    EXPECT_FALSE(arc.release_reference());
    t2_done.store(true);
    t2_done.notify_all();
    done.wait(false);
  });

  t1_done.wait(false);
  t2_done.wait(false);

  EXPECT_FALSE(arc.initiate_shutdown());
  EXPECT_TRUE(arc.release_reference());

  done.store(true);
  done.notify_all();
}

} // namespace hogd
