#include <cassert>
#include <iostream>

#include "x23/cleanup.h"

using namespace x23;

static_assert(is_nullable<std::function<void()>>); // T{} not constexpr
static_assert(is_nullable<void (*)()>);            // T{} constexpr

Defer<>
set_true1(bool *b)
{
  assert(!*b);
  return {[b] { *b = true; }};
}

Defer<>
set_true2(bool *b)
{
  assert(!*b);
  return [b] { *b = true; };
}

Defer<>
set_true3(bool *b)
{
  assert(!*b);
  Defer d = [b] { *b = true; };
  return d;
}

Defer<>
set_once(bool *b)
{
  return Defer([b, ran = false]() mutable {
    assert(!ran);
    ran = true;
    *b = true;
  });
}

void
basic()
{
  bool success = false;
  {
    Defer _c([&] {
      assert(!success);
      success = true;
    });
  }
  assert(success);

  success = false;
  {
    Defer<> _c([&] {
      assert(!success);
      success = true;
    });
  }
  assert(success);

  success = false;
  {
    auto d = set_true1(&success);
    assert(d);
  }
  assert(success);

  success = false;
  {
    auto d = set_true2(&success);
    assert(d);
  }
  assert(success);

  success = false;
  {
    auto d = set_true2(&success);
    assert(d);
    d.release();
    assert(!d);
  }
  assert(!success);

  success = false;
  {
    auto d = set_true3(&success);
    assert(d);
  }
  assert(success);

  success = false;
  {
    Defer d = set_once(&success);
    assert(d);
  }
  assert(success);

  struct rvalue_only {
    int val{};

    rvalue_only() = default;
    rvalue_only(rvalue_only &&) = default;

    void operator()() &&
    {
      ++val;
      assert(val == 1);
    }
  };
  rvalue_only ro;
  {
    Defer d1{std::move(ro)};
    auto d2 = std::move(d1);
  }
}

int
main()
{
  basic();
  // Defer([]() { std::cout << "discarded\n"; });
  return 0;
}
