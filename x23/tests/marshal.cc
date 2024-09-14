
#include <cassert>
#include <print>

#include <x23/serialize.h>

#include "tests/xdrtest.hh"

using namespace x23;
using std::uint8_t;

static_assert(detail::has_fixed_size<fix_4>());
static_assert(detail::has_fixed_size<fix_12>());
static_assert(detail::has_fixed_size<fix_16>());
static_assert(detail::has_fixed_size<sunion_no_void>());
static_assert(!detail::has_fixed_size<sunion>());
static_assert(detail::xdr_size_bounds<sunion>() == detail::SizeBounds{4,8});
static_assert(detail::xdr_size_bounds<test_recursive>() ==
	      detail::SizeBounds{12,detail::kNoBound});
static_assert(xdr_size(fix_4{}) == 4);
static_assert(xdr_size(fix_12{}) == 12);
static_assert(xdr_size(fix_16{}) == 16);

void
check_sizes()
{
  u_4_12 u(4);
  assert(xdr_size(u) == 8);
  u.which(12);
  assert(xdr_size(u) == 16);

  v12 v{{1, 1.0}, {2, 2.0}, {3, 3.0}};
  assert(xdr_size(v) == 40);

  uptr upu(false);
  assert(xdr_size(upu) == 4);
  upu.b(true);
  assert(xdr_size(upu) == 8);
  upu.val().emplace();
  assert(xdr_size(upu) == 12);

  coloru cu(RED);
  assert(xdr_size(cu) == 8);
  cu.r() = "x";
  assert(xdr_size(cu) == 12);
  cu.c(REDDER);
  assert(xdr_size(cu) == 12);
}

void
round_trip()
{
  fix_4 f4 { .i = 99 };
  auto buf = serialize(f4);
  assert(buf.size() == 4);

  fix_4 f4prime;
  deserialize(&f4prime, buf);
  assert(f4 == f4prime);

  u_4_12 u(4), uprime;
  u.f4().i = 26;
  buf = serialize(u);
  assert(buf.size() == 8);
  deserialize(&uprime, buf);
  assert(u == uprime);
}

int
main()
{
  check_sizes();
  round_trip();
}
