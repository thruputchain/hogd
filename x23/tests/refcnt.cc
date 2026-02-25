
#include <gtest/gtest.h>

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
