
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
#include <thread>

#include <x23/debug.h>
#include <x23/refcnt.h>

using namespace x23; // NOLINT

// NOLINTNEXTLINE
std::atomic<void (*)(std::atomic_int *)> use =
    +[](std::atomic_int *p) { *p = 0xb285c774; };

struct Thing : RefCount {
  using RefCount::refcount;
  static constexpr bool kAllowRefPtrConversion = true;
  static constexpr int kMagic = 0x0652a08f;
  std::atomic_int good = kMagic;
  ~Thing()
  {
    x23assert(good == kMagic);
    use.load()(&good);
  }
};

// RefPtr<T> can't be a member of T (RefOps eagerly needs complete type),
// so split into a complete base and a derived class that adds the link.
struct ListNode : RefCount {
  using RefCount::refcount;
  static constexpr bool kAllowRefPtrConversion = true;
};
struct Link : ListNode {
  RefPtr<ListNode> next;
};

// head = head->next where head is sole owner: copy-assign unrefs head
// before reffing next, but unref destroys the node (and its next member).
TEST(refcnt, DISABLED_CopyAssignLinkedListAdvance)
{
  auto *tail = new Link;
  RefPtr<ListNode> keep(*tail);

  auto *h = new Link;
  h->next = keep;
  RefPtr<ListNode> head(*h);

  head = h->next; // head = head->next
  EXPECT_EQ(head.get(), tail);
}

TEST(refcnt, copy)
{
  RefPtr<Thing> c1(*new Thing), c2, c3;
  EXPECT_EQ(c1->refcount(), 1UZ);
  c2 = c1;
  EXPECT_EQ(c1->refcount(), 2UZ);
  c3 = c2;
  EXPECT_EQ(c1->refcount(), 3UZ);
  c1 = nullptr;
  EXPECT_EQ(c3->refcount(), 2UZ);
  c2 = nullptr;
  EXPECT_EQ(c3->refcount(), 1UZ);
  c3 = nullptr;
}

TEST(refcnt, destroy_inhibit_wait_after_last_ref_release)
{
  auto obj = std::make_unique<DestroyInhibit>();
  DestroyInhibit *raw = obj.get();

  RefPtr<DestroyInhibit> ref(*raw);
  std::atomic<bool> dropped{false};
  std::atomic<bool> done{false};

  std::jthread t([ref = std::move(ref), &dropped, &done]() mutable {
    ref.reset();
    dropped.store(true, std::memory_order_relaxed);
    while (!done.load(std::memory_order_relaxed)) {
      std::this_thread::yield();
    }
  });

  while (!dropped.load(std::memory_order_relaxed)) {
    std::this_thread::yield();
  }

  raw->destroy_wait();
  obj.reset();

  done.store(true, std::memory_order_relaxed);
  t.join();
}

struct DestroyInhibitPayload : DestroyInhibit {
  static constexpr bool kAllowRefPtrConversion = true;

  int value = 0;

  ~DestroyInhibitPayload() { EXPECT_EQ(value, 1); }
};

TEST(refcnt, destroy_inhibit_synchronizes_prior_writes_after_last_ref_release)
{
  auto obj = std::make_unique<DestroyInhibitPayload>();
  DestroyInhibitPayload *raw = obj.get();

  RefPtr<DestroyInhibitPayload> ref(*raw);
  std::atomic<bool> dropped{false};

  std::jthread t([ref = std::move(ref), raw, &dropped]() mutable {
    raw->value = 1;
    ref.reset();
    dropped.store(true, std::memory_order_relaxed);
  });

  while (!dropped.load(std::memory_order_relaxed)) {
    std::this_thread::yield();
  }

  raw->destroy_wait();
  obj.reset();

  t.join();
}

TEST(refcnt, destroy_inhibit_synchronizes_prior_writes_after_prior_last_ref)
{
  auto obj = std::make_unique<DestroyInhibitPayload>();
  DestroyInhibitPayload *raw = obj.get();

  RefPtr<DestroyInhibitPayload> ref(*raw);
  std::atomic<bool> done{false};

  std::jthread t([ref = std::move(ref), raw, &done]() mutable {
    raw->value = 1;
    ref.reset();
    while (!done.load(std::memory_order_relaxed)) {
      std::this_thread::yield();
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  raw->destroy_wait();
  EXPECT_EQ(raw->value, 1);
  obj.reset();

  done.store(true, std::memory_order_relaxed);
}

TEST(refcnt, destroy_inhibit_synchronizes_storage_reuse_after_prior_last_ref)
{
  alignas(DestroyInhibit) std::byte storage[sizeof(DestroyInhibit)];
  auto *raw = new (storage) DestroyInhibit();

  RefPtr<DestroyInhibit> ref(*raw);
  std::atomic<bool> done{false};

  std::jthread t([ref = std::move(ref), &done]() mutable {
    ref.reset();
    while (!done.load(std::memory_order_relaxed)) {
      std::this_thread::yield();
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  raw->destroy_wait();
  raw->~DestroyInhibit();
  std::memset(storage, 0xab, sizeof(storage));

  done.store(true, std::memory_order_relaxed);
}

TEST(refcnt, destroy_inhibit_synchronizes_storage_reuse_after_last_ref_release)
{
  alignas(DestroyInhibit) std::byte storage[sizeof(DestroyInhibit)];
  auto *raw = new (storage) DestroyInhibit();

  RefPtr<DestroyInhibit> ref(*raw);
  std::atomic<bool> waiter_started{false};
  std::atomic<bool> waiter_done{false};

  std::jthread waiter([raw, &storage, &waiter_started, &waiter_done] {
    waiter_started.store(true, std::memory_order_relaxed);
    raw->destroy_wait();
    raw->~DestroyInhibit();
    std::memset(storage, 0xab, sizeof(storage));
    waiter_done.store(true, std::memory_order_relaxed);
  });

  while (!waiter_started.load(std::memory_order_relaxed)) {
    std::this_thread::yield();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  ref.reset();

  while (!waiter_done.load(std::memory_order_relaxed)) {
    std::this_thread::yield();
  }
}

TEST(refcnt, destroy_inhibit_waits_for_concurrent_last_ref_release)
{
  auto obj = std::make_unique<DestroyInhibit>();
  DestroyInhibit *raw = obj.get();

  RefPtr<DestroyInhibit> ref(*raw);
  std::atomic<bool> release{false};

  std::jthread t([ref = std::move(ref), &release]() mutable {
    while (!release.load(std::memory_order_relaxed)) {
      std::this_thread::yield();
    }
    ref.reset();
  });

  release.store(true, std::memory_order_relaxed);
  raw->destroy_wait();
  obj.reset();

  t.join();
}
