
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

// Simulates the scenario where `other` in copy-assignment is reachable from
// the old pointee.  When unref destroys objectA, its destructor resets the
// global alias (standing in for a member RefPtr), so the copy-assignment
// reads a dangling/nulled `other.p_` if it unrefs old before reffing new.
namespace {
struct Poisoner : RefCount {
  using RefCount::refcount;
  static constexpr bool kAllowRefPtrConversion = true;
  bool armed = false;
  ~Poisoner();
};

RefPtr<Poisoner> g_alias; // declared after Poisoner is complete

Poisoner::~Poisoner()
{
  if (armed)
    g_alias.reset(); // simulates ~RefPtr on a member
}
} // namespace

TEST(refcnt, CopyAssignSafeWhenOtherInvalidatedByUnref)
{
  RefPtr<Poisoner> b(*new Poisoner);
  Poisoner *raw_b = b.get();
  g_alias = b; // g_alias → objectB, refcount = 2

  auto *objA = new Poisoner;
  objA->armed = true;        // objectA's destructor will reset g_alias
  RefPtr<Poisoner> a(*objA); // a → objectA, refcount = 1

  // BUG: unref(A) → ~Poisoner(armed) → g_alias.reset() → g_alias.p_ = null
  // then p_ = other.p_ reads null
  a = g_alias;
  EXPECT_EQ(a.get(), raw_b);
  EXPECT_EQ(b->refcount(), 2UZ); // a + b both hold objectB

  g_alias = nullptr; // clean up
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
