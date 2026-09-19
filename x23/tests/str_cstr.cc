#define X23_CSTRING_DEBUG

#include <cstdint>
#include <cstring>

#include <gtest/gtest.h>
#include <string_view>
#include <utility>

#include "x23/vec.h"

// Verify that c_str() returns a properly NUL-terminated string whose
// strlen matches the Str's size().  Each test exercises a different
// mutation path through Vec/BoundedStr.
namespace {
void
check_cstr(const auto &s, size_t expected_size)
{
  ASSERT_EQ(s.size(), expected_size);
  const char *p = s.c_str();
  EXPECT_EQ(std::strlen(p), expected_size);
}
} // namespace

TEST(StrCstr, Construct)
{
  x23::Str s = "hello";
  check_cstr(s, 5);
}

TEST(StrCstr, PushBack)
{
  x23::Str s;
  s.push_back('a');
  s.push_back('b');
  s.push_back('c');
  check_cstr(s, 3);
}

TEST(StrCstr, PushBackMany)
{
  x23::Str s;
  for (int i = 0; i < 100; i++)
    s.push_back('x');
  check_cstr(s, 100);
}

TEST(StrCstr, Reserve)
{
  x23::Str s = "abc";
  s.reserve(1000);
  check_cstr(s, 3);
}

TEST(StrCstr, PushBackThenReserve)
{
  x23::Str s;
  for (int i = 0; i < 50; i++)
    s.push_back('y');
  s.reserve(10000);
  check_cstr(s, 50);
}

TEST(StrCstr, ResizeSmaller)
{
  x23::Str s = "hello world";
  s.resize(5);
  check_cstr(s, 5);
}

TEST(StrCstr, ResizeLargerFill)
{
  x23::Str s = "hi";
  s.resize(10, 'x');
  check_cstr(s, 10);
}

TEST(StrCstr, OperatorPlusEquals)
{
  x23::Str s = "hello";
  s += std::string_view(" world");
  check_cstr(s, 11);
}

TEST(StrCstr, AppendRange)
{
  x23::Str s = "abc";
  std::string_view suffix = "def";
  s.append_range(suffix);
  check_cstr(s, 6);
}

TEST(StrCstr, Clear)
{
  x23::Str s = "hello";
  s.clear();
  const char *p = s.c_str();
  EXPECT_EQ(std::strlen(p), 0U);
}

TEST(StrCstr, DefaultConstructed)
{
  x23::Str s;
  const char *p = s.c_str();
  EXPECT_EQ(std::strlen(p), 0U);
}

TEST(StrCstr, EraseRange)
{
  x23::Str s = "hello world";
  s.erase(s.begin() + 5, s.end());
  check_cstr(s, 5);
}

TEST(StrCstr, EraseSingle)
{
  x23::Str s = "hello";
  s.erase(s.begin() + 2);
  check_cstr(s, 4);
}

TEST(StrCstr, PopBack)
{
  x23::Str s = "hello";
  s.pop_back();
  check_cstr(s, 4);
}

TEST(StrCstr, ShrinkToFit)
{
  x23::Str s = "hello";
  s.reserve(1000);
  s.shrink_to_fit();
  check_cstr(s, 5);
}

TEST(StrCstr, OperatorPlus)
{
  x23::Str s = x23::Str("hello") + x23::Str(" world");
  check_cstr(s, 11);
}

TEST(StrCstr, SubstrPos0)
{
  x23::Str s = "hello world";
  x23::Str sub = std::move(s).substr(0, 5);
  check_cstr(sub, 5);
}

TEST(StrCstr, SubstrMemmove)
{
  x23::Str s = "hello world";
  x23::Str sub = std::move(s).substr(6);
  check_cstr(sub, 5);
}

TEST(StrCstr, SubstrCopyThenMove)
{
  x23::Str s = "hello world";
  x23::Str copy = s;
  x23::Str sub = std::move(copy).substr(6);
  check_cstr(sub, 5);
  check_cstr(s, 11);
}

TEST(StrCstr, BoundedAtCapacity)
{
  x23::BoundedStr<4> s = "1234";
  check_cstr(s, 4);
}

TEST(StrCstr, BoundedPushBackToCapacity)
{
  x23::BoundedStr<4> s;
  s.push_back('a');
  s.push_back('b');
  s.push_back('c');
  s.push_back('d');
  check_cstr(s, 4);
}

TEST(StrCstr, Hexdump)
{
  x23::Vec<uint8_t> data;
  data.push_back(0xab);
  data.push_back(0xcd);
  x23::Str hex = hexdump(data);
  check_cstr(hex, 4);
}

TEST(StrCstr, Uformat)
{
  x23::Str s = x23::uformat("{}{}{}", "hello", " ", "world");
  check_cstr(s, 11);
}

TEST(StrCstr, UformatTo)
{
  x23::Str s = "prefix";
  x23::uformat_to(&s, "_{}", 42);
  check_cstr(s, 9);
}

TEST(StrCstr, CopyAssign)
{
  x23::Str a = "hello";
  x23::Str b = "goodbye world";
  b = a;
  check_cstr(b, 5);
}

TEST(StrCstr, MoveAssign)
{
  x23::Str a = "hello";
  x23::Str b;
  b = std::move(a);
  check_cstr(b, 5);
}

TEST(StrCstr, ReinterpretRoundTrip)
{
  x23::Str s = "hello";
  auto v = std::move(s).reinterpret<x23::Vec<unsigned char>>();
  auto s2 = std::move(v).reinterpret<x23::Str>();
  check_cstr(s2, 5);
}

// Reinterpret from a Vec<uint8_t> that was never a string — the byte
// at *b_.end has no reason to be '\0' unless terminate() is called.
TEST(StrCstr, ReinterpretFromVecU8)
{
  x23::Vec<uint8_t> v;
  for (uint8_t c : {'h', 'e', 'l', 'l', 'o'})
    v.push_back(c);
  auto s = std::move(v).reinterpret<x23::Str>();
  check_cstr(s, 5);
}

// An unallocated Vec has null begin/end/bufend.  Reinterpreting to a type
// with a smaller kExtraAlloc must not offset the null bufend, or the result
// claims capacity over a null buffer and the next insert writes through it.
TEST(StrCstr, ReinterpretEmptyKeepsNullBuffer)
{
  x23::Str s;
  auto v = std::move(s).reinterpret<x23::Vec<unsigned char>>();
  EXPECT_EQ(v.data(), nullptr);
  EXPECT_EQ(v.size(), 0U);
  EXPECT_EQ(v.capacity(), 0U);
  v.push_back('x');
  EXPECT_EQ(v.size(), 1U);
  EXPECT_EQ(v[0], 'x');

  x23::Vec<unsigned char> empty;
  auto s2 = std::move(empty).reinterpret<x23::Str>();
  EXPECT_EQ(s2.size(), 0U);
  s2.push_back('y');
  check_cstr(s2, 1);
}

// Construct a Str directly via the Buf-based constructor with a
// poisoned byte at *b_.end.  Verifies the constructor itself calls
// terminate(), rather than relying on callers.
TEST(StrCstr, BufConstructorTerminates)
{
  using A = x23::Str::allocator_type;
  A a;
  constexpr size_t n = 5;
  auto *raw = a.allocate(n + x23::Str::kExtraAlloc);
  auto *p = reinterpret_cast<char *>(raw);
  std::memcpy(p, "hello", n);
  p[n] = 'X'; // poison the terminator position
  x23::Str s(x23::Str::Buf{.begin = p, .end = p + n, .bufend = p + n}, a);
  check_cstr(s, 5);
}

TEST(StrCstr, EmplaceBackReturnValue)
{
  x23::Str s;
  char &ref = s.emplace_back('z');
  EXPECT_EQ(ref, 'z');
  EXPECT_EQ(&ref, &s.back());
}
