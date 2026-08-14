#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>
#include <utility>

#include "hogd/base/compiler.h"
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

// Regression (issue #3217): race initiate_shutdown() against the release of
// the last outstanding reference. Once initiate_shutdown() publishes readiness
// the last releaser is authorized to destroy the ARC, so the initiator must
// retain ownership across its own remaining accesses to the object (notably
// is_shutdown_ready_.notify_all()). Whichever party drains the pending count to
// zero owns destruction; exactly one of the two must claim it, and teardown
// must be clean (the ARC destructor asserts shutdown_pending_refcount_ == 0).
TEST(PerThreadARCTest, InitiateShutdownRacesWithLastRelease)
{
  for (int iter = 0; iter < 1000; iter++) {
    auto *arc = new PerThreadARC<int>(0);

    std::atomic<int> barrier{0};
    std::atomic<bool> acquired{false};
    std::atomic<bool> shutdown_done{false};
    std::atomic<int> owners{0};

    std::jthread worker([&] {
      ASSERT_TRUE(arc->try_acquire_reference());
      acquired.store(true, std::memory_order_release);
      acquired.notify_all();

      barrier.fetch_add(1, std::memory_order_release);
      while (barrier.load(std::memory_order_acquire) < 2) {
      }
      if (arc->release_reference()) {
        owners.fetch_add(1, std::memory_order_relaxed);
        delete arc;
      }
      // Stay alive until initiate_shutdown() has drained this thread's slot, so
      // TLS teardown never observes a live refcount.
      shutdown_done.wait(false, std::memory_order_acquire);
    });

    acquired.wait(false, std::memory_order_acquire);

    barrier.fetch_add(1, std::memory_order_release);
    while (barrier.load(std::memory_order_acquire) < 2) {
    }
    if (arc->initiate_shutdown()) {
      owners.fetch_add(1, std::memory_order_relaxed);
      delete arc;
    }
    shutdown_done.store(true, std::memory_order_release);
    shutdown_done.notify_all();

    worker.join();
    EXPECT_EQ(owners.load(std::memory_order_relaxed), 1)
        << "iteration " << iter;
  }
}

TEST(PerThreadARCTest, ReleaseSynchronizesBeforeRemoteSlotTeardown)
{
  auto arc = std::make_unique<PerThreadARC<int>>(0);
  PerThreadARC<int> *raw_arc = arc.get();
  ASSERT_TRUE(raw_arc->try_acquire_reference());

  std::atomic<bool> released{false};
  bool shutdown_result = false;

  std::jthread t([&] {
    while (!released.load(std::memory_order_relaxed)) {
      std::this_thread::yield();
    }

    shutdown_result = raw_arc->initiate_shutdown();
    arc.reset();
  });

  EXPECT_FALSE(raw_arc->release_reference());
  released.store(true, std::memory_order_relaxed);
  t.join();

  EXPECT_TRUE(shutdown_result);
}

// Repro for the CI use-after-free that aborts RpcLoadgenEventTest /
// ERC20ExperimentTest teardown at perthread_arc.h:243
// ("CHECK FAILED: count_.load(...) <= 0").
//
// A PerThreadARC keeps each touching thread's refcount in a thread_local slot
// (PerThread<Refcount>). A balanced acquire/release leaves that slot at
// kInitializedValue, a *positive* sentinel; the slot is only driven
// non-positive (to kShutdownValue) by initiate_shutdown()'s for_each over the
// live threads. If a thread that touched the ARC exits before
// initiate_shutdown() has drained its slot, thread_local teardown
// (detail::TLStorage::remove() -> ~TLItems -> ~Refcount()) observes the live
// positive count and aborts on CHECK(count_ <= 0).
//
// Production hits this as a teardown race: an RPC/DB worker thread exits while
// a BlockTX's PerThreadARC<BlockTX> is still live (not yet shut down), which is
// why it only fails intermittently under load. This test forces the same
// invariant violation deterministically so the abort reproduces without relying
// on scheduler timing -- the existing regression tests above all take pains to
// keep their worker threads alive until shutdown precisely to avoid it.
//
// It is a death test because the defect manifests as a process abort, which can
// only be observed from a subprocess. Once the lifetime bug is fixed (a thread
// that touched the ARC may exit before shutdown without tripping the assert)
// this expectation should be replaced with one that asserts a clean exit.
TEST(PerThreadARCTest, ThreadExitBeforeShutdownTripsRefcountAssert)
{
  // Under NDEBUG the CHECK lowers to std::unreachable(), so it emits no
  // "CHECK FAILED" message (and the failure path may be optimized away). Match
  // the other CHECK-based death tests and only run this in debug builds.
  if constexpr (!is_debug_build()) {
    GTEST_SKIP() << "CHECK is compiled out under NDEBUG";
  }
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(
      {
        // Leaked deliberately: the ARC must still be alive (never shut down)
        // when the worker thread exits.
        auto *arc = new PerThreadARC<int>(0);
        std::thread worker([&] {
          // A worker fully processes its work: acquire then release. The
          // release returns false (not the last reference, no shutdown), so
          // this thread's slot is left at kInitializedValue > 0.
          arc->try_acquire_reference();
          arc->release_reference();
        });
        // Thread exit runs TLStorage::remove() -> ~Refcount() with a live slot.
        worker.join();
      },
      "CHECK FAILED: count_.load");
}

} // namespace hogd
