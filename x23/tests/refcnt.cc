
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
#include <set>
#include <thread>
#include <utility>

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

// Both RefPtr assignment operators used to drop the reference they held
// before capturing the source.  These tests check that the source is now
// captured before destroying the old pointee can mutate it or free its
// storage.
//
// See issue #1158; PR #1157 landed CopyAssignLinkedListAdvance as a
// DISABLED_ reproducer.
//
// RefPtr<T> can't be a member of T (RefOps eagerly needs complete type),
// so split into a complete base and derived classes that add state.
// The base destructor is virtual because RefCount::finalize() deletes
// through the base pointer the RefPtr holds; without it the derived
// destructor never runs and ASAN reports a new-delete-type-mismatch that
// has nothing to do with the bug under test.
struct ListNode : RefCount {
  using RefCount::refcount;
  static constexpr bool kAllowRefPtrConversion = true;
  virtual ~ListNode() = default;
};
struct Link : ListNode {
  RefPtr<ListNode> next;
};

// Destroying a Clearer nulls a RefPtr held elsewhere -- a stand-in for
// any destructor with side effects on refcounted state.  Nothing is
// freed out from under the assignment here, so the old decrement-first
// ordering made these tests fail deterministically without a sanitizer.
struct Clearer : ListNode {
  RefPtr<ListNode> *victim = nullptr;
  ~Clearer() override
  {
    if (victim)
      *victim = nullptr;
  }
};

TEST(refcnt, CopyAssignSourceMutatedByOldObjectDestructor)
{
  auto *tail = new Link;
  RefPtr<ListNode> keep(*tail);
  RefPtr<ListNode> src = keep;

  auto *c = new Clearer;
  c->victim = &src;
  RefPtr<ListNode> head(*c);

  head = src; // head retains tail before ~Clearer nulls src
  EXPECT_EQ(head.get(), tail);
}

TEST(refcnt, MoveAssignSourceMutatedByOldObjectDestructor)
{
  auto *tail = new Link;
  RefPtr<ListNode> keep(*tail);
  RefPtr<ListNode> src = keep;

  auto *c = new Clearer;
  c->victim = &src;
  RefPtr<ListNode> head(*c);

  head = std::move(src); // src is moved before ~Clearer assigns nullptr
  EXPECT_EQ(head.get(), tail);
}

// Here head is the sole owner of a node containing the source RefPtr.
// Assignment must capture the source before releasing head destroys that
// node.  The old decrement-first ordering caused a heap-use-after-free.
TEST(refcnt, CopyAssignLinkedListAdvance)
{
  auto *tail = new Link;
  RefPtr<ListNode> keep(*tail);

  auto *h = new Link;
  h->next = keep;
  RefPtr<ListNode> head(*h);

  head = h->next; // head = head->next
  EXPECT_EQ(head.get(), tail);
}

TEST(refcnt, MoveAssignLinkedListAdvance)
{
  auto *tail = new Link;
  RefPtr<ListNode> keep(*tail);

  auto *h = new Link;
  h->next = keep;
  RefPtr<ListNode> head(*h);

  head = std::move(h->next); // head = std::move(head->next)
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

TEST(refcnt, AtomicCompareExchangeRetainsAndReplacesReferences)
{
  RefPtr first(*new Thing), second(*new Thing);
  AtomicRefPtr<Thing> atomic(first);
  auto desired = second;
  EXPECT_FALSE(atomic.compare_exchange(desired, second.get()));
  EXPECT_EQ(desired.get(), first.get());
  EXPECT_EQ(first->refcount(), 3UZ);
  EXPECT_EQ(second->refcount(), 1UZ);
  desired = second;
  EXPECT_TRUE(atomic.compare_exchange(desired, first.get()));
  EXPECT_EQ(desired.get(), second.get());
  EXPECT_EQ(first->refcount(), 1UZ);
  auto old = atomic.exchange(nullptr);
  EXPECT_EQ(old.get(), second.get());
  EXPECT_FALSE(atomic.load());
  EXPECT_FALSE(atomic.compare_exchange(desired, second.get()));
  EXPECT_FALSE(desired);
  EXPECT_EQ(second->refcount(), 2UZ);
  EXPECT_TRUE(atomic.compare_exchange(first, nullptr));
  EXPECT_EQ(atomic.load().get(), first.get());
}

namespace {

struct TaggedRefs {
  static constexpr bool kAllowRefPtrConversion = true;
  int next_{};
  std::set<int> tags_;
  int incref() noexcept
  {
    tags_.insert(++next_);
    return next_;
  }
  void decref(int tag) noexcept { EXPECT_EQ(tags_.erase(tag), 1UZ); }
};

} // namespace

TEST(refcnt, TaggedReferencesFollowCopyMoveSwapAndAtomicOwnership)
{
  TaggedRefs a, b;
  {
    RefPtr first(a), second(b);
    auto copy = first;
    EXPECT_EQ(a.tags_.size(), 2UZ);
    swap(first, second);
    EXPECT_EQ(first.get(), &b);
    EXPECT_EQ(second.get(), &a);
    copy = std::move(first);
    EXPECT_FALSE(first);
    EXPECT_EQ(a.tags_.size(), 1UZ);
    EXPECT_EQ(b.tags_.size(), 1UZ);
    auto &alias = copy;
    copy = alias;
    copy = std::move(alias);
    AtomicRefPtr<TaggedRefs> atomic(std::move(copy));
    auto loaded = atomic.load();
    EXPECT_EQ(loaded.get(), &b);
    EXPECT_EQ(b.tags_.size(), 2UZ);
    EXPECT_TRUE(atomic.compare_exchange(second, &b));
    EXPECT_EQ(a.tags_.size(), 2UZ);
    EXPECT_EQ(b.tags_.size(), 1UZ);
    atomic.store(nullptr);
  }
  EXPECT_TRUE(a.tags_.empty());
  EXPECT_TRUE(b.tags_.empty());
}

TEST(refcnt, AtomicOpsAdaptTaggedAndUntaggedReferences)
{
  RefPtr owner(*new Thing);
  std::atomic<Thing *> pointer(owner.get());
  using Ops = detail::AtomicRefOps<Thing, RefPtr<Thing>::ops>;
  EXPECT_EQ(Ops::get(pointer), owner.get());
  Ops::incref(pointer);
  EXPECT_EQ(owner->refcount(), 2UZ);
  Ops::decref(pointer);
  EXPECT_EQ(owner->refcount(), 1UZ);

  TaggedRefs tagged;
  std::atomic<TaggedRefs *> tagged_pointer(&tagged);
  using TaggedOps = detail::AtomicRefOps<TaggedRefs, RefPtr<TaggedRefs>::ops>;
  auto tag = TaggedOps::incref(tagged_pointer);
  EXPECT_TRUE(tagged.tags_.contains(tag));
  EXPECT_EQ(TaggedOps::get(tagged_pointer), &tagged);
  TaggedOps::decref(tagged_pointer, tag);
  EXPECT_TRUE(tagged.tags_.empty());
}

TEST(refcnt, CopyingEmptyReferenceReleasesPreviousObject)
{
  RefPtr<Thing> empty;
  auto copy = empty;
  EXPECT_FALSE(copy);
  RefPtr owner(*new Thing);
  auto assigned = owner;
  assigned = empty;
  EXPECT_FALSE(assigned);
  EXPECT_EQ(owner->refcount(), 1UZ);
}
