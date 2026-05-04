
#include <cassert>
#include <cstdint>
#include <print>

#include <x23/serialize.h>
#include <x23/vec.h>

#include "tests/xdrtest.hh"

using namespace x23;
using std::uint8_t;

static_assert(detail::has_fixed_size<fix_4>());
static_assert(detail::has_fixed_size<fix_12>());
static_assert(detail::has_fixed_size<fix_16>());
static_assert(detail::has_fixed_size<sunion_no_void>());
static_assert(!detail::has_fixed_size<sunion>());
static_assert(detail::xdr_size_bounds<sunion>() == detail::SizeBounds{4, 8});
static_assert(detail::xdr_size_bounds<test_recursive>() ==
              detail::SizeBounds{12, detail::kNoBound});
static_assert(detail::xdr_size_bounds<const ConstUnionBugStruct>() ==
              detail::xdr_size_bounds<ConstUnionBugStruct>());
static_assert(detail::xdr_size_bounds<const struct_with_union>() ==
              detail::xdr_size_bounds<struct_with_union>());
static_assert(detail::xdr_size_bounds<const MutualFrame>() ==
              detail::xdr_size_bounds<MutualFrame>());
static_assert(xdr_size(fix_4{}) == 4);
static_assert(xdr_size(fix_12{}) == 12);
static_assert(xdr_size(fix_16{}) == 16);

static_assert(!is_recursive_xdr<senum>);
static_assert(!is_recursive_xdr<sunion>);
static_assert(!is_recursive_xdr<fix_16>);
static_assert(detail::xdr_contains<fix_4, int32_t>());
static_assert(!is_recursive_xdr<fix_4>);
static_assert(!is_recursive_xdr<uunion>);
static_assert(is_recursive_xdr<test_recursive>);
static_assert(!is_recursive_xdr<contains_test_recursive>);
static_assert(contains_recursive_xdr<contains_test_recursive>);
static_assert(contains_recursive_xdr<test_recursive>);
static_assert(!contains_recursive_xdr<uunion>);

static_assert(x23::is_xdr_type<x23::Box<fix_4>>);
static_assert(x23::is_xdr_cat<x23::Box<fix_4>, x23::XdrCategory::kStruct>);
static_assert(std::tuple_size_v<x23::Box<fix_4>> == 1);
static_assert(x23::is_xdr_serializable<x23::Box<fix_4>>);

// box_tree itself is not directly recursive (its fields are Box<box_tree>,
// not box_tree), but Box<box_tree> is recursive and box_tree contains it.
static_assert(!x23::is_recursive_xdr<box_tree>);
static_assert(x23::is_recursive_xdr<x23::Box<box_tree>>);
static_assert(x23::contains_recursive_xdr<box_tree>);
static_assert(x23::detail::xdr_size_bounds<box_tree>() ==
              x23::detail::SizeBounds{12, x23::detail::kNoBound});

struct my_fix_4 {
  int32_t i;
};
inline size_t
xdr_size(const my_fix_4 &)
{
  return 4;
}
void
xdr_serialize(auto &serializer, const my_fix_4 &f)
{
  serializer.put32(f.i);
}

static_assert(x23::detail::has_custom_serializer<my_fix_4>);
static_assert(x23::is_xdr_serializable<my_fix_4>);

void
check_sizes()
{
  using enum color;
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
  fix_4 f4{.i = 99};
  auto buf = serialize(f4);
  assert(buf.size() == 4);

  fix_4 f4prime;
  deserialize(&f4prime, buf);
  assert(f4 == f4prime);

  my_fix_4 mf4{.i = f4.i};
  auto buf2 = serialize(mf4);
  assert(buf == buf2);

  {
    x23::span s(&mf4, 1);
    buf2 = serialize(s);
    assert(buf == x23::Buf(std::from_range, buf2.subspan(4)));
  }
  {
    x23::span s(&f4, 1);
    buf = serialize(s);
    assert(buf == buf2);
  }
  {
    Ptr<XdrVec<fix_4>> f4vp;
    f4vp.emplace(XdrVec<fix_4>{{.i = 555}});
    x23::span f4s(*f4vp);
    BorrowedPtr bp(&f4s);
    buf = serialize(f4vp);
    buf2 = serialize(bp);
    assert(buf == buf2);
  }

  u_4_12 u(4), uprime;
  u.f4().i = 26;
  buf = serialize(u);
  assert(buf.size() == 8);
  deserialize(&uprime, buf);
  assert(u == uprime);

  test_recursive r, r2;
  r.elem = "outer element";
  r.nextvec.resize(1);
  r.nextvec[0].elem = "vec element";
  buf = serialize(r);
  deserialize(&r2, buf);
  assert(r == r2);

  fix_12 f12{.i = 99, .d = 95.3}, f12b;
  buf = serialize(f12);
  deserialize(&f12b, buf);
  assert(f12 == f12b);
}

void
projected_view()
{
  // Project fix_12::i (fixed-size member) from a vec of fix_12.
  // The projected span should serialize identically to a Vec<int32_t>.
  XdrVec<fix_12> v12s{{.i = 10, .d = 1.0},
                      {.i = 20, .d = 2.0},
                      {.i = 30, .d = 3.0}};
  ProjectedView<fix_12, &fix_12::i> proj(v12s);
  auto buf = serialize(proj);

  XdrVec<int32_t> ints{10, 20, 30};
  auto buf2 = serialize(ints);
  assert(buf == buf2);

  // Verify serialize_equiv so it can be used where Vec<int32_t> is expected.
  static_assert(
      serializes_like<ProjectedView<fix_12, &fix_12::i>, Vec<int32_t>>);

  // Project middle_struct::inner (fixed-size member from variable-size struct).
  XdrVec<middle_struct> mids{
      {.inner = {.x = 1, .y = 2.0}, .tag = "hello"},
      {.inner = {.x = 3, .y = 4.0}, .tag = "world"},
  };
  ProjectedView<middle_struct, &middle_struct::inner> proj2(mids);
  buf = serialize(proj2);

  XdrVec<inner_struct> expected{{.x = 1, .y = 2.0}, {.x = 3, .y = 4.0}};
  buf2 = serialize(expected);
  assert(buf == buf2);

  // Empty span.
  XdrVec<fix_12> empty;
  ProjectedView<fix_12, &fix_12::i> proj_empty(empty);
  buf = serialize(proj_empty);
  XdrVec<int32_t> empty_ints;
  buf2 = serialize(empty_ints);
  assert(buf == buf2);

  // Fixed-extent: no size prefix, serializes like std::array.
  std::array<fix_12, 2> arr{fix_12{.i = 7, .d = 7.0}, fix_12{.i = 8, .d = 8.0}};
  ProjectedView<fix_12, &fix_12::i, 2> proj_fixed(arr);
  buf = serialize(proj_fixed);

  std::array<int32_t, 2> ints_arr{7, 8};
  buf2 = serialize(ints_arr);
  assert(buf == buf2);

  static_assert(serializes_like<ProjectedView<fix_12, &fix_12::i, 2>,
                                std::array<int32_t, 2>>);
}

void
check_box()
{
  using namespace x23;

  fix_12 plain{.i = 42, .d = 3.14};
  Box<fix_12> boxed(plain);
  // Box serializes with a 4-byte presence flag, so size = 4 + inner size.
  assert(xdr_size(boxed) == 4 + xdr_size(plain));

  Buf boxed_buf = serialize(boxed);

  Box<fix_12> deserialized;
  deserialize(&deserialized, boxed_buf);
  assert(*deserialized == plain);

  Box<fix_12> copied = boxed;
  assert(*copied == *boxed);
  Box<fix_12> moved = std::move(copied);
  assert(*moved == plain);

  Box<fix_12> a(fix_12{.i = 1, .d = 0.0});
  Box<fix_12> b(fix_12{.i = 2, .d = 0.0});
  assert(a < b);
  assert(a != b);
  Box<fix_12> c(fix_12{.i = 1, .d = 0.0});
  assert(a == c);

  // Default-constructed Box is empty (absent) until first access.
  Box<fix_12> empty;
  assert(xdr_size(empty) == 4);
}

int
main()
{
  check_sizes();
  round_trip();
  projected_view();
  check_box();
}
