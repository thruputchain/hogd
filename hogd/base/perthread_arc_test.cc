#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <utility>

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

// Test PerThreadARCRef basic usage
TEST(PerThreadARCRefTest, BasicUsage)
{
  PerThreadARC<int> arc(42);
  EXPECT_TRUE(arc.try_acquire_reference());

  {
    PerThreadARCRef<int> ref(&arc);

    // Test pointer-like dereference
    EXPECT_EQ(*ref, 42);
    *ref = 100;
    EXPECT_EQ(*ref, 100);

    // Test arrow operator
    int *ptr = ref.operator->();
    EXPECT_EQ(*ptr, 100);

    // Test bool conversion
    EXPECT_TRUE(ref);

    // Release reference before destruction
    EXPECT_FALSE(ref.release_reference());
  }

  EXPECT_TRUE(arc.initiate_shutdown());
}

TEST(PerThreadARCRefTest, NullPointer)
{
  PerThreadARCRef<int> ref(nullptr);

  // Null pointer should be false in bool context
  EXPECT_FALSE(ref);

  // Should not be active, so destructor should not assert
  // (active_ is false for null pointer)
}

TEST(PerThreadARCRefTest, MoveSemantics)
{
  PerThreadARC<int> arc(99);
  EXPECT_TRUE(arc.try_acquire_reference());

  {
    PerThreadARCRef<int> ref1(&arc);
    EXPECT_EQ(*ref1, 99);
    EXPECT_TRUE(ref1);

    // Move constructor
    PerThreadARCRef<int> ref2(std::move(ref1));
    EXPECT_EQ(*ref2, 99);
    EXPECT_TRUE(ref2);
    // ref1 is moved-from (active_ is false), but arc_ is still non-null
    // so bool conversion still returns true, but destructor won't assert

    // Move assignment
    PerThreadARCRef<int> ref3(nullptr);
    EXPECT_FALSE(ref3);
    ref3 = std::move(ref2);
    EXPECT_EQ(*ref3, 99);
    EXPECT_TRUE(ref3);
    // ref2 is moved-from, but arc_ is still non-null

    EXPECT_FALSE(ref3.release_reference());
  }

  EXPECT_TRUE(arc.initiate_shutdown());
}

TEST(PerThreadARCRefTest, ConstAccess)
{
  PerThreadARC<int> arc(123);
  EXPECT_TRUE(arc.try_acquire_reference());

  PerThreadARCRef<int> ref(&arc);

  {

    auto const &rr = ref;

    // Test const dereference
    EXPECT_EQ(*rr, 123);

    // Test const arrow operator
    const int *ptr = rr.operator->();
    EXPECT_EQ(*ptr, 123);
  }

  EXPECT_FALSE((ref.release_reference()));

  EXPECT_TRUE(arc.initiate_shutdown());
}

} // namespace hogd
