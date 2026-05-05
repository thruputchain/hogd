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

    EXPECT_FALSE(ref2.release_reference());
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

// Regression: release_reference() on a fresh thread (one that has never touched
// this ARC) transiently sets the threadlocal refcount to kUninitializedValue-1
// via fetch_sub before acquiring the thread lock. If initiate_shutdown()
// iterates threadlocals in that window, it must tolerate this transient value.
TEST(PerThreadARCTest, ShutdownRacesWithCrossThreadRelease)
{
  for (int iter = 0; iter < 1000; iter++) {
    PerThreadARC<int> arc(0);
    ASSERT_TRUE(arc.try_acquire_reference());

    std::atomic<int> barrier{0};
    std::atomic<bool> t_released{false};
    std::atomic<bool> shutdown_done{false};

    std::jthread t([&] {
      barrier.fetch_add(1, std::memory_order_release);
      while (barrier.load(std::memory_order_acquire) < 2) {
      }
      bool r = arc.release_reference();
      t_released.store(r, std::memory_order_release);
      // Keep thread alive until shutdown completes so TLS cleanup
      // sees kShutdownValue (Refcount destructor asserts count_ <= 0).
      shutdown_done.wait(false, std::memory_order_acquire);
    });

    barrier.fetch_add(1, std::memory_order_release);
    while (barrier.load(std::memory_order_acquire) < 2) {
    }
    bool shutdown_result = arc.initiate_shutdown();
    shutdown_done.store(true, std::memory_order_release);
    shutdown_done.notify_all();

    t.join();

    bool t_rel = t_released.load(std::memory_order_acquire);
    EXPECT_NE(shutdown_result, t_rel) << "iteration " << iter;
  }
}

// Same race but for try_acquire_reference: fetch_add on a fresh thread
// transiently sets the threadlocal to kUninitializedValue+1 before the lock.
TEST(PerThreadARCTest, ShutdownRacesWithCrossThreadAcquire)
{
  for (int iter = 0; iter < 1000; iter++) {
    PerThreadARC<int> arc(0);

    std::atomic<int> barrier{0};
    std::atomic<bool> shutdown_done{false};

    std::jthread t([&] {
      barrier.fetch_add(1, std::memory_order_release);
      while (barrier.load(std::memory_order_acquire) < 2) {
      }
      if (arc.try_acquire_reference()) {
        arc.release_reference();
      }
      shutdown_done.wait(false, std::memory_order_acquire);
    });

    barrier.fetch_add(1, std::memory_order_release);
    while (barrier.load(std::memory_order_acquire) < 2) {
    }
    arc.initiate_shutdown();
    shutdown_done.store(true, std::memory_order_release);
    shutdown_done.notify_all();
    t.join();
    // PerThreadARC destructor validates shutdown_pending_refcount_ == 0.
  }
}

// Verify that kUninitializedValue±2 is structurally impossible: the first
// try_acquire or release from a fresh thread completes the slow path (which
// initializes the threadlocal to kInitializedValue±1) before returning.
// A second sequential op on the same thread therefore never starts from
// kUninitializedValue, so the threadlocal cannot transiently hold ±2.
// All ops from T complete before initiate_shutdown to avoid the ±1 race.
TEST(PerThreadARCTest, SequentialOpsFromFreshThreadCannotReachPlusMinus2)
{
  {
    PerThreadARC<int> arc(0);
    std::atomic<bool> ops_done{false};
    std::atomic<bool> shutdown_done{false};
    std::jthread t([&] {
      EXPECT_TRUE(arc.try_acquire_reference());
      EXPECT_TRUE(arc.try_acquire_reference());
      EXPECT_FALSE(arc.release_reference());
      EXPECT_FALSE(arc.release_reference());
      ops_done.store(true, std::memory_order_release);
      ops_done.notify_all();
      shutdown_done.wait(false, std::memory_order_acquire);
    });
    ops_done.wait(false, std::memory_order_acquire);
    EXPECT_TRUE(arc.initiate_shutdown());
    shutdown_done.store(true, std::memory_order_release);
    shutdown_done.notify_all();
    t.join();
  }
  {
    PerThreadARC<int> arc(0);
    ASSERT_TRUE(arc.try_acquire_reference());
    ASSERT_TRUE(arc.try_acquire_reference());
    std::atomic<bool> ops_done{false};
    std::atomic<bool> shutdown_done{false};
    std::jthread t([&] {
      EXPECT_FALSE(arc.release_reference());
      EXPECT_FALSE(arc.release_reference());
      ops_done.store(true, std::memory_order_release);
      ops_done.notify_all();
      shutdown_done.wait(false, std::memory_order_acquire);
    });
    ops_done.wait(false, std::memory_order_acquire);
    EXPECT_TRUE(arc.initiate_shutdown());
    shutdown_done.store(true, std::memory_order_release);
    shutdown_done.notify_all();
    t.join();
  }
}

} // namespace hogd
