// Checks x23c --c output against the C++ codec, using random values of the
// types in c_backend.x:
//
//   - C decodes what C++ encodes, to the same values, and re-encodes it to
//     the same bytes (including through the lazily decoded views).
//   - C encodes values it builds itself (native arrays, not views) to the
//     bytes C++ encodes.
//   - For corrupted encodings, C accepts exactly what C++ accepts, except
//     that C (like Rust) rejects bool union discriminants other than 0 or 1,
//     and anything C accepts it re-encodes to the same bytes.
//   - C bounds view nesting (X23_NESTING_LIMIT).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <span>
#include <type_traits>
#include <vector>

#include "tests/c_backend.hh"
#include "x23/serialize.h"

// After the C++ header, whose constants the C header's macros would rename.
#include "tests/c_backend.h"

namespace {

uint64_t seed;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "%s:%d: CHECK(%s) failed (seed %llu)\n", __FILE__,  \
                   __LINE__, #cond, (unsigned long long)seed);                 \
      std::abort();                                                            \
    }                                                                          \
  } while (0)

#define CHECK_OK(expr) CHECK((expr) == X23_OK)

using Bytes = std::vector<uint8_t>;

Bytes
to_bytes(const x23::Buf &b)
{
  return Bytes(b.data(), b.data() + b.size());
}

// Random C++ values, walking the x23 type traits.
struct Randomizer {
  static constexpr int kMaxDepth = 6;
  std::mt19937_64 rng;
  int depth = 0;

  explicit Randomizer(uint64_t s) : rng(s) {}

  uint64_t r() { return rng(); }
  size_t len(size_t max, size_t cap) { return r() % (std::min(max, cap) + 1); }

  template<typename T> void go(x23::Box<T> &b)
  {
    T v;
    go(v);
    b = x23::Box<T>(std::move(v));
  }

  template<typename D> D discriminant()
  {
    if constexpr (std::is_same_v<D, bool>)
      return r() & 1;
    else if constexpr (x23::is_xdr_cat<D, x23::XdrCategory::kEnum>) {
      constexpr auto &tags = x23::xdr_meta<D>::tags;
      return tags[r() % tags.size()].first;
    }
    else
      return D(r() % 5);
  }

  template<typename X> void go(X &x)
  {
    using enum x23::XdrCategory;
    if constexpr (std::is_same_v<X, bool>)
      x = r() & 1;
    else if constexpr (x23::is_xdr_cat<X, kEnum>)
      x = discriminant<X>();
    else if constexpr (std::is_floating_point_v<X>) {
      // Finite, so that values compare equal to themselves.
      x = X(int64_t(r() % 200001) - 100000) / 64;
    }
    else if constexpr (std::is_integral_v<X>)
      x = X(r());
    else if constexpr (x23::is_xdr_cat<X, kOpaqueArray>)
      for (auto &b : x)
        b = uint8_t(r());
    else if constexpr (x23::is_xdr_cat<X, kString, kOpaqueVec>) {
      size_t n = len(x.max_size(), 20);
      if constexpr (requires { x.resize_uninitialized(n); })
        x.resize_uninitialized(n);
      else
        x.resize(n);
      for (size_t i = 0; i < n; ++i)
        x.data()[i] =
            std::remove_cvref_t<decltype(x.data()[0])>('a' + r() % 26);
    }
    else if constexpr (x23::is_xdr_cat<X, kArray>)
      for (auto &e : x)
        go(e);
    else if constexpr (x23::is_xdr_cat<X, kVec>) {
      x.resize(depth >= kMaxDepth ? 0 : len(x.max_size(), 4));
      for (auto &e : x)
        go(e);
    }
    else if constexpr (x23::is_xdr_cat<X, kPtr>) {
      bool present = depth < kMaxDepth && (r() & 1);
      x.resize(present);
      if (present)
        go(*x);
    }
    else if constexpr (x23::is_xdr_struct<X>) {
      ++depth;
      x23::visit_fields([&](auto &f) { go(f); }, x);
      --depth;
    }
    else if constexpr (x23::is_xdr_union<X>) {
      ++depth;
      using D = typename x23::xdr_meta<X>::discriminant_type;
      for (int tries = 0;; ++tries) {
        CHECK(tries < 1000);
        D d = discriminant<D>();
        if (x23::union_ops<X>::index(d) < 0)
          continue;
        x23::union_set_discriminant(x, d);
        // Past the depth limit, only arms that cannot recurse.
        if (depth >= kMaxDepth &&
            x23::union_visit_arm(
                []<typename A>(A &) {
                  return x23::contains_recursive_xdr<std::remove_cvref_t<A>>;
                },
                x, false))
          continue;
        break;
      }
      x23::union_visit_arm([&](auto &a) { go(a); }, x);
      --depth;
    }
    else
      static_assert(false, "unhandled XDR type");
  }
};

bool
same(x23_bytes c, std::span<const uint8_t> x)
{
  return c.len == x.size() &&
         (x.empty() || std::memcmp(c.data, x.data(), x.size()) == 0);
}

bool
same(x23_string c, std::string_view x)
{
  return c.len == x.size() &&
         (x.empty() || std::memcmp(c.data, x.data(), x.size()) == 0);
}

// Encodes v with the C encoder, first checking that a counting writer and a
// too-small buffer behave.
template<typename T>
Bytes
c_encode(const T &v,
         x23_status (*to_xdr)(const T *, uint8_t *, uint32_t, uint32_t *))
{
  uint32_t n = 0;
  CHECK_OK(to_xdr(&v, nullptr, 0, &n));
  Bytes out(n + 4);
  uint32_t got = 0;
  if (n >= 4)
    CHECK(to_xdr(&v, out.data(), n - 4, &got) == X23_ERR_OVERFLOW);
  CHECK_OK(to_xdr(&v, out.data(), n, &got));
  CHECK(got == n);
  out.resize(n);
  return out;
}

// Decoded C values against the C++ values they came from.  Each also
// checks that the C value re-encodes to the C++ encoding.

void
check(const Point &c, const ctest::Point &x)
{
  CHECK(c.x == x.x && c.y == x.y);
  CHECK(c_encode(c, Point_to_xdr) == to_bytes(x23::serialize(x)));
}

void
check(const Node &c, const ctest::Node &x)
{
  CHECK(c.value == x.value);
  CHECK(Node_opt_present(&c.next) == bool(x.next));
  if (x.next) {
    Node next;
    CHECK_OK(Node_opt_get(&c.next, &next));
    check(next, *x.next);
  }
  CHECK(c_encode(c, Node_to_xdr) == to_bytes(x23::serialize(x)));
}

void
check(const Shape &c, const ctest::Shape &x)
{
  CHECK(c.kind == int32_t(x.kind()));
  switch (x.kind()) {
  case ctest::Color::RED:
    check(c.center, x.center());
    break;
  case ctest::Color::GREEN:
  {
    CHECK(c.corners.len == x.corners().size());
    Point_vec_iter it;
    Point_vec_iter_init(&it, &c.corners);
    Point p;
    for (size_t i = 0; i < x.corners().size(); ++i) {
      CHECK(Point_vec_next(&it, &p));
      check(p, x.corners()[i]);
    }
    CHECK(!Point_vec_next(&it, &p));
    break;
  }
  case ctest::Color::BLUE:
    break;
  }
  CHECK(c_encode(c, Shape_to_xdr) == to_bytes(x23::serialize(x)));
}

void
check(const TypeT &c, const ctest::TypeT &x)
{
  CHECK(c.k == int32_t(x.k()));
  TypeT inner;
  switch (x.k()) {
  case ctest::Kind::K_VOID:
    break;
  case ctest::Kind::K_OPT:
    CHECK_OK(TypeT_box_get(&c.inner, &inner));
    check(inner, *x.inner());
    break;
  case ctest::Kind::K_ARRAY:
    CHECK(c.array.len == x.array().len);
    CHECK_OK(TypeT_box_get(&c.array.element, &inner));
    check(inner, *x.array().element);
    break;
  case ctest::Kind::K_TUPLE:
    CHECK(c.tuple.len == x.tuple().size());
    // Backwards, so that each get walks from the start.
    for (size_t i = x.tuple().size(); i-- > 0;) {
      CHECK_OK(TypeT_vec_get(&c.tuple, uint32_t(i), &inner));
      check(inner, x.tuple()[i]);
    }
    CHECK(TypeT_vec_get(&c.tuple, c.tuple.len, &inner) == X23_ERR_RANGE);
    break;
  }
  CHECK(c_encode(c, TypeT_to_xdr) == to_bytes(x23::serialize(x)));
}

void
check(const Everything &c, const ctest::Everything &x)
{
  const auto &p = x.prims;
  CHECK(c.prims.i == p.i && c.prims.u == p.u && c.prims.h == p.h &&
        c.prims.uh == p.uh && c.prims.b == p.b && c.prims.f == p.f &&
        c.prims.d == p.d && c.prims.c == int32_t(p.c) &&
        c.prims.hue == int32_t(p.hue) && c.prims.shade == int32_t(p.shade));
  CHECK(std::memcmp(c.hash.bytes, x.hash.data(), 32) == 0);
  CHECK(std::memcmp(c.odd.bytes, x.odd.data(), 5) == 0);
  CHECK(same(c.blob, x.blob));
  CHECK(same(c.any, x.any));
  CHECK(same(c.name, x.name));
  CHECK(same(c.label, x.label));
  for (int i = 0; i < 3; ++i)
    check(c.fixed[i], x.fixed[i]);
  CHECK(c.ints[0] == x.ints[0] && c.ints[1] == x.ints[1]);
  CHECK(c.vints.len == x.vints.size());
  for (uint32_t i = 0; i < c.vints.len; ++i) {
    int32_t v;
    CHECK_OK(x23_i32_vec_get(&c.vints, i, &v));
    CHECK(v == x.vints[i]);
  }
  // Fixed-size elements: indexed directly.
  CHECK(c.points.len == x.points.size());
  for (uint32_t i = 0; i < c.points.len; ++i) {
    Point pt;
    CHECK_OK(Point_vec_get(&c.points, i, &pt));
    check(pt, x.points[i]);
  }
  // Variable-size elements: walked.
  CHECK(c.shapes.len == x.shapes.size());
  for (uint32_t i = 0; i < c.shapes.len; ++i) {
    Shape s;
    CHECK_OK(Shape_vec_get(&c.shapes, i, &s));
    check(s, x.shapes[i]);
  }
  CHECK(c.labels.len == x.labels.size());
  for (uint32_t i = 0; i < c.labels.len; ++i) {
    Label l;
    CHECK_OK(Label_vec_get(&c.labels, i, &l));
    CHECK(same(l, x.labels[i]));
  }
  CHECK(c.num.k == x.num.k());
  if (x.num.k() == 1)
    CHECK(c.num.big == x.num.big());
  else if (x.num.k() == 2 || x.num.k() == 3)
    CHECK(c.num.small == x.num.small());
  CHECK(c.flag.on == x.flag.on());
  if (x.flag.on())
    CHECK(same(c.flag.name, x.flag.name()));
  CHECK(Node_opt_present(&c.list) == bool(x.list));
  if (x.list) {
    Node n;
    CHECK_OK(Node_opt_get(&c.list, &n));
    check(n, *x.list);
  }
  CHECK(Point_opt_present(&c.maybe) == bool(x.maybe));
  if (x.maybe) {
    Point pt;
    CHECK_OK(Point_opt_get(&c.maybe, &pt));
    check(pt, *x.maybe);
  }
  check(c.type, x.type);
  CHECK(c.anon.a == x.anon.a);
  CHECK(c.anon.inner.z == x.anon.inner.z());
  if (x.anon.inner.z())
    CHECK(c.anon.inner.t == x.anon.inner.t());
}

// C values built natively from C++ values: arrays and pointers rather than
// views of encoded bytes.  The arena keeps them alive.
struct Arena {
  std::vector<std::shared_ptr<void>> keep;
  template<typename T> T *alloc(size_t n)
  {
    T *p = new T[n ? n : 1]();
    keep.emplace_back(p, [](void *q) { delete[] static_cast<T *>(q); });
    return p;
  }
};

x23_bytes
c_bytes(std::span<const uint8_t> x)
{
  return {x.data(), uint32_t(x.size())};
}

x23_string
c_string(std::string_view x)
{
  return {x.data(), uint32_t(x.size())};
}

Point
to_c(const ctest::Point &x, Arena &)
{
  return {x.x, x.y};
}

Node
to_c(const ctest::Node &x, Arena &a)
{
  Node c{};
  c.value = x.value;
  if (x.next) {
    Node *n = a.alloc<Node>(1);
    *n = to_c(*x.next, a);
    c.next.ptr = n;
  }
  return c;
}

Shape
to_c(const ctest::Shape &x, Arena &a)
{
  Shape c{};
  c.kind = int32_t(x.kind());
  if (x.kind() == ctest::Color::RED)
    c.center = to_c(x.center(), a);
  else if (x.kind() == ctest::Color::GREEN) {
    Point *ps = a.alloc<Point>(x.corners().size());
    for (size_t i = 0; i < x.corners().size(); ++i)
      ps[i] = to_c(x.corners()[i], a);
    c.corners.len = uint32_t(x.corners().size());
    c.corners.elems = ps;
  }
  return c;
}

TypeT
to_c(const ctest::TypeT &x, Arena &a)
{
  TypeT c{};
  c.k = int32_t(x.k());
  switch (x.k()) {
  case ctest::Kind::K_VOID:
    break;
  case ctest::Kind::K_OPT:
  {
    TypeT *t = a.alloc<TypeT>(1);
    *t = to_c(*x.inner(), a);
    c.inner.ptr = t;
    break;
  }
  case ctest::Kind::K_ARRAY:
  {
    TypeT *t = a.alloc<TypeT>(1);
    *t = to_c(*x.array().element, a);
    c.array.element.ptr = t;
    c.array.len = x.array().len;
    break;
  }
  case ctest::Kind::K_TUPLE:
  {
    TypeT *ts = a.alloc<TypeT>(x.tuple().size());
    for (size_t i = 0; i < x.tuple().size(); ++i)
      ts[i] = to_c(x.tuple()[i], a);
    c.tuple.len = uint32_t(x.tuple().size());
    c.tuple.elems = ts;
    break;
  }
  }
  return c;
}

template<typename CV, typename XV>
auto
to_c_vec(const XV &x, Arena &a)
{
  using E = std::remove_cvref_t<decltype(*CV{}.elems)>;
  E *es = a.alloc<E>(x.size());
  for (size_t i = 0; i < x.size(); ++i) {
    if constexpr (std::is_same_v<E, Label>)
      es[i] = c_string(x[i]);
    else if constexpr (std::is_arithmetic_v<E>)
      es[i] = x[i];
    else
      es[i] = to_c(x[i], a);
  }
  CV v{};
  v.len = uint32_t(x.size());
  v.elems = es;
  return v;
}

Everything
to_c(const ctest::Everything &x, Arena &a)
{
  Everything c{};
  const auto &p = x.prims;
  c.prims = {p.i, p.u, p.h,          p.uh,           p.b,
             p.f, p.d, int32_t(p.c), int32_t(p.hue), int32_t(p.shade)};
  std::memcpy(c.hash.bytes, x.hash.data(), 32);
  std::memcpy(c.odd.bytes, x.odd.data(), 5);
  c.blob = c_bytes(x.blob);
  c.any = c_bytes(x.any);
  c.name = c_string(x.name);
  c.label = c_string(x.label);
  for (int i = 0; i < 3; ++i)
    c.fixed[i] = to_c(x.fixed[i], a);
  c.ints[0] = x.ints[0];
  c.ints[1] = x.ints[1];
  c.vints = to_c_vec<Ints>(x.vints, a);
  c.points = to_c_vec<Point_vec>(x.points, a);
  c.shapes = to_c_vec<Shape_vec>(x.shapes, a);
  c.labels = to_c_vec<Label_vec>(x.labels, a);
  c.num.k = x.num.k();
  if (x.num.k() == 1)
    c.num.big = x.num.big();
  else if (x.num.k() == 2 || x.num.k() == 3)
    c.num.small = x.num.small();
  c.flag.on = x.flag.on();
  if (x.flag.on())
    c.flag.name = c_string(x.flag.name());
  if (x.list) {
    Node *n = a.alloc<Node>(1);
    *n = to_c(*x.list, a);
    c.list.ptr = n;
  }
  if (x.maybe) {
    Point *pt = a.alloc<Point>(1);
    *pt = to_c(*x.maybe, a);
    c.maybe.ptr = pt;
  }
  c.type = to_c(x.type, a);
  c.anon.a = x.anon.a;
  c.anon.inner.z = x.anon.inner.z();
  if (x.anon.inner.z())
    c.anon.inner.t = x.anon.inner.t();
  return c;
}

template<typename X>
bool
cpp_accepts(const Bytes &b)
{
  X v;
  try {
    x23::Buf buf(b.data(), b.data() + b.size());
    x23::deserialize(&v, x23::BufViewEnd(buf));
    return true;
  } catch (const x23::XdrException &) {
    return false;
  }
}

template<typename X>
Bytes
cpp_reencode(const Bytes &b)
{
  X v;
  x23::Buf buf(b.data(), b.data() + b.size());
  x23::deserialize(&v, x23::BufViewEnd(buf));
  return to_bytes(x23::serialize(v));
}

// Corrupts an encoding and checks that C accepts what C++ does.
void
mutate(std::mt19937_64 &rng, const Bytes &good)
{
  Bytes b = good;
  switch (rng() % 5) {
  case 0: // one byte
    b[rng() % b.size()] = uint8_t(rng());
    break;
  case 1: // a word: huge, a bad bool or length, or zero
    if (b.size() >= 4) {
      static constexpr uint32_t kWords[] = {0xffffffff, 2, 0, 1, 0x80000000};
      size_t w = rng() % (b.size() / 4);
      uint32_t v = kWords[rng() % std::size(kWords)];
      for (int i = 0; i < 4; ++i)
        b[4 * w + i] = uint8_t(v >> (24 - 8 * i));
    }
    break;
  case 2: // truncated
    b.resize(rng() % b.size());
    break;
  case 3: // trailing data
    b.resize(b.size() + 4 * (1 + rng() % 2));
    break;
  case 4: // misaligned
    b.push_back(0);
    break;
  }
  bool cpp_ok = cpp_accepts<ctest::Everything>(b);
  Everything c;
  x23_status st = Everything_from_xdr(b.data(), uint32_t(b.size()), &c);
  if (st == X23_OK) {
    CHECK(cpp_ok);
    CHECK(c_encode(c, Everything_to_xdr) == b);
  }
  else if (cpp_ok) {
    // C++ accepts a bool union discriminant other than 0 or 1, which C (like
    // Rust) rejects as it does any other bad bool: an encoding C++ would not
    // produce.
    CHECK(st == X23_ERR_BOOL && cpp_reencode<ctest::Everything>(b) != b);
  }
}

void
random_tests(int n)
{
  for (int i = 0; i < n; ++i) {
    seed = 0x5eed0000 + i;
    Randomizer rnd(seed);
    ctest::Everything x;
    rnd.go(x);
    Bytes b = to_bytes(x23::serialize(x));

    Everything c;
    CHECK_OK(Everything_from_xdr(b.data(), uint32_t(b.size()), &c));
    check(c, x);
    CHECK(c_encode(c, Everything_to_xdr) == b);

    Arena a;
    Everything native = to_c(x, a);
    CHECK(c_encode(native, Everything_to_xdr) == b);

    for (int j = 0; j < 20; ++j)
      mutate(rnd.rng, b);
  }
}

// Each optional or box level counts against the nesting limit, so a chain
// of n values nests n - 1 deep.
void
nesting_limit_tests()
{
  seed = 0;
  for (size_t n = 1; n <= X23_NESTING_LIMIT + 3; ++n) {
    ctest::Node list{.value = 0};
    ctest::Node *tail = &list;
    for (size_t i = 1; i < n; ++i) {
      tail->next = x23::Ptr(ctest::Node{.value = int32_t(i)});
      tail = &*tail->next;
    }
    Bytes b = to_bytes(x23::serialize(list));
    Node c;
    x23_status st = Node_from_xdr(b.data(), uint32_t(b.size()), &c);
    CHECK(st == (n - 1 <= X23_NESTING_LIMIT ? X23_OK : X23_ERR_DEPTH));

    ctest::TypeT t(ctest::Kind::K_VOID);
    for (size_t i = 1; i < n; ++i) {
      ctest::TypeT outer(ctest::Kind::K_OPT);
      outer.inner() = x23::Box<ctest::TypeT>(std::move(t));
      t = std::move(outer);
    }
    b = to_bytes(x23::serialize(t));
    TypeT ct;
    st = TypeT_from_xdr(b.data(), uint32_t(b.size()), &ct);
    CHECK(st == (n - 1 <= X23_NESTING_LIMIT ? X23_OK : X23_ERR_DEPTH));
  }
}

void
misc_tests()
{
  seed = 0;
  // Fixed sizes: known where the encoding cannot vary.
  static_assert(Point_XDR_SIZE == 8);
  static_assert(Hash_XDR_SIZE == 32);
  static_assert(Odd_XDR_SIZE == 8);
  static_assert(Color_XDR_SIZE == 4);
  static_assert(Prims_XDR_SIZE == 4 * 3 + 8 * 3 + 4 * 4);
  static_assert(Shape_XDR_SIZE == 0);
  static_assert(Label_XDR_SIZE == 0);

  // Enum constants are prefixed, so reusing a name across enums is fine.
  static_assert(Color_RED == 0 && Shade_RED == 7 && Color_BLUE == -5);

  // An enum holds any int32, as in C++ and Rust; a union discriminant must
  // still select an arm.
  static_assert(Color_XDR_SIZE == 4);
  CHECK(!Shade_valid(0) && Shade_valid(Shade_DARK));
  const uint8_t shade_99[] = {0, 0, 0, 99};
  Shade sh;
  CHECK_OK(Shade_from_xdr(shade_99, 4, &sh));
  CHECK(sh == 99);
  uint32_t n;
  CHECK_OK(Shade_to_xdr(&sh, nullptr, 0, &n));
  CHECK(cpp_accepts<ctest::Shade>(Bytes(shade_99, shade_99 + 4)));
  Shape s{};
  s.kind = 3;
  CHECK(Shape_to_xdr(&s, nullptr, 0, &n) == X23_ERR_UNION);
  Everything e{};
  CHECK_OK(Everything_to_xdr(&e, nullptr, 0, &n));
  e.type.k = Kind_K_OPT; // a box holding nothing
  CHECK(Everything_to_xdr(&e, nullptr, 0, &n) == X23_ERR_RANGE);
  e.type.k = Kind_K_VOID;
  e.name.len = 11; // over the bound of 10
  e.name.data = "01234567890";
  CHECK(Everything_to_xdr(&e, nullptr, 0, &n) == X23_ERR_LENGTH);

  // Specific bad encodings.
  const uint8_t bad_bool[] = {0, 0, 0, 1, 0, 0, 0, 2};
  Flag f;
  CHECK(Flag_from_xdr(bad_bool + 4, 4, &f) == X23_ERR_BOOL);
  const uint8_t bad_pad[] = {0, 0, 0, 1, 'a', 0, 0, 1};
  x23_bytes bv;
  x23_reader r = x23_reader_init(bad_pad, sizeof(bad_pad));
  CHECK(x23_bytes_decode(&r, &bv, 8) == X23_ERR_PADDING);
  Point pt;
  CHECK(Point_from_xdr(bad_pad, 7, &pt) == X23_ERR_ALIGN);
  CHECK(Point_from_xdr(bad_pad, 4, &pt) == X23_ERR_SHORT);
}

} // namespace

int
main()
{
  misc_tests();
  nesting_limit_tests();
  random_tests(5000);
  std::printf("c_backend: ok\n");
  return 0;
}
