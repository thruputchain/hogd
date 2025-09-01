
#include <cassert>
#include <exception>
#include <print>
#include <ranges>
#include <vector>

#include "x23/vec.h"

using namespace x23;

#if !BROKEN_P2564R3

constexpr size_t test_constexpr = [] {
  auto v = "123"_ustr;
  v.resize(5, '0');
  v.resize(7);
  v.resize(4);
  v.append_range(v);
  Str v2;
  v2.append("12301230", 8);
  if (v.size() != 8)
    throw std::runtime_error("Uvec test failure");
  if (v.size() != v2.size())
    throw std::runtime_error("Uvec test failure");
  if (v != v2)
    throw std::runtime_error("Uvec test failure");
  return v.size();
}();

static_assert(test_constexpr == 8);

#endif // !BROKEN_P2564R3

template<typename T>
inline bool
operator==(const Vec<T> &u, const std::vector<T> &v)
{
  return std::equal(u.begin(), u.end(), v.begin(), v.end());
}

template<typename T, typename It>
void
append(std::vector<T> &v, It first, size_t count)
{
  v.insert(v.end(), first, first + count);
}

template<typename T, typename It>
void
append(Vec<T> &v, It first, size_t count)
{
  v.append(first, first + count);
}

void
append_range(auto &v, auto &&r)
{
  if constexpr (requires { v.append_range(r); })
    v.append_range(r);
  else {
    v.reserve(v.size() + std::ranges::size(r));
    v.insert(v.end(), r.begin(), r.end());
  }
}

template<typename T>
void
uvec_vs_vector(const T &init = {})
{
  Vec<T> u;
  std::vector<T, std::allocator<T>> v;

  auto doboth = [&u, &v](auto &&f) {
    f(u);
    f(v);
    assert(u == v);
  };

  doboth([&init](auto &v) { append(v, &init, 1); });
  doboth([&init](auto &v) { v.resize(3, init); });
  doboth([](auto &v) { v.resize(5); });
  doboth([](auto &v) { append_range(v, v); });
  doboth([](auto &v) { v.resize(v.size() - 1); });
}

void
reinterpret()
{
  Str x = "Hello world\n"_ustr;
  auto y = auto(x).reinterpret<Vec<unsigned char>>();
  assert(!y.empty());
  auto z = std::move(y).reinterpret<Str>();
  assert(y.empty()); // wiped by reinterpret
  assert(x == z);
}

void
add()
{
  auto x = Str("Hello ") + Str("World");
  static_assert(std::is_same_v<decltype(x), Str>);
  assert(x == Str("Hello World"));
}

void
str_methods()
{
  auto x = "Hello world!\n"_ustr;
  assert(x.contains("world"));
  assert(x.contains("world"_ustr));
  assert(!x.contains("goodbye"_ustr));
  assert(x.find("world") == 6);
  assert(x.find("goodbye") == x.npos);
}

void
format_test()
{
  Str x;
  uformat_to(&x, "{}", 7);
  std::println("{}", x);
  uformat_to(&x, "{}", 8);
  std::println("{}", x);
  uformat_to(&x, "{}", 9);
  std::println("{}", x);
  uformat_to(&x, "{}", std::string{"abcdefg"});

  std::println("{}", x);

  assert(x == "789abcdefg"_ustr);
}

int
main()
{
  uvec_vs_vector(7);
  uvec_vs_vector(std::make_shared<int>(8));
  add();
  reinterpret();
  format_test();
}
