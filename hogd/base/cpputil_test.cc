#include "hogd/base/cpputil.h"
#include <gtest/gtest.h>

namespace hogd {

namespace {

// must be polymorphic
class A {
  virtual void foo() {}
};

class B : public A {};

class C : public B {};

} // namespace

TEST(CppUtil, MostDerived)
{
  C derived;
  A &base = derived;
  EXPECT_TRUE(most_derived_cast<B>(&base) == nullptr);
  EXPECT_TRUE(most_derived_cast<C>(&base) != nullptr);
}

} // namespace hogd
