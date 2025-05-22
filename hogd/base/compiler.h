// compiler.h - low-level compiler utilities

#pragma once

#include <concepts>
#include <new>
#include <type_traits>
#include <utility>

#include "hogd/base/types.h"

namespace hogd {

// CSTR - converts a preprocessor value to a string
#define CSTR_T(x...) #x
#define CSTR(x...) CSTR_T(x)

// HAS_ATTRIBUTE - returns true if an attribute is supported
#ifdef __has_attribute
#define HAS_ATTRIBUTE(x) __has_attribute(x)
#else
#define HAS_ATTRIBUTE(x) 0
#endif

// variable attributes
#define ATTRIBUTE_PACKED __attribute__((packed))

// function attributes
#define ATTRIBUTE_NOINLINE __attribute__((noinline))
#define ATTRIBUTE_ALWAYS_INLINE __attribute__((always_inline))
#define ATTRIBUTE_COLD __attribute__((cold))

//
// Prefetching support
//

// prefetch0 prefetches data into all levels of the cache hierarchy
inline void
prefetch0(const void *addr)
{
  __builtin_prefetch(addr, 0, 3);
}

// prefetch1 pretetches data into the L2 or deeper
inline void
prefetch1(const void *addr)
{
  __builtin_prefetch(addr, 0, 2);
}

// prefetch2 prefetches data into the L2 or deeper
inline void
prefetch2(const void *addr)
{
  __builtin_prefetch(addr, 0, 1);
}

// prefetch_nta prefetches data nontemporally
inline void
prefetch_nta(const void *addr)
{
  __builtin_prefetch(addr, 0, 0);
}

// prefetch prefetches data into the cache at the default level
inline void
prefetch(const void *addr)
{
  prefetch0(addr);
}

// Likely and unlikely (more general than the C++ ones, works with ternaries)
#ifndef likely
#define likely(x) __builtin_expect(!!(x), 1)
#endif
#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

// is_debug_build returns true if the build is in debug mode. This can be used
// to conditionally enable additional debugging features.
constexpr bool
is_debug_build()
{
#ifndef NDEBUG
  return true;
#else
  return false;
#endif
}

// ASSERT_CONSTEXPR is assert(), but it works in constexpr functions
// See:
// https://akrzemi1.wordpress.com/2017/05/18/asserts-in-constexpr-functions/
// https://github.com/abseil/abseil-cpp/blob/master/absl/base/macros.h
#if defined NDEBUG
#define ASSERT_CONSTEXPR(x)                                                    \
  (false ? static_cast<void>(x) : static_cast<void>(0))
#else
#define ASSERT_CONSTEXPR(x)                                                    \
  (likely(x) ? static_cast<void>(0) : [] { assert(false && #x); }()) // NOLINT
#endif

// barrier prevents the compiler from reordering code across it
inline void
barrier()
{
  asm volatile("" ::: "memory");
}

//
// Memory access overrides
//
// These prevent memory accesses from being optimized out. They should not be
// used for inter-thread communication (use std::mutex or std::atomic for
// that), but there are situations like communication with signal handlers,
// restartable sequences, or hardware devices where they can be useful.
//

// access_once forces the compiler to access (read or write) a memory location.
template<typename T>
T volatile &
access_once(T &t)
{
  return static_cast<T volatile &>(t);
}

// read_once forces the compiler to read a memory location.
template<typename T>
T
read_once(const T &p)
{
  return static_cast<const T volatile &>(p);
}

// write_once force the compiler to write a memory location.
template<typename T>
void
write_once(T &p, const T &val)
{
  static_cast<T volatile &>(p) = val;
}

// cpu_relax hints to the CPU that it is in a spin loop.
//
// Example:
// while (condition)
//   cpu_relax();
inline void
cpu_relax()
{
#if HAVE_BUILTIN_IA32_PAUSE
  __builtin_ia32_pause();
#endif
  // TODO(amb): figure out what is appropriate on arm and figure out if there
  // are cases where we want UMWAIT/TPAUSE on x86.
}

// overflowing_add returns a pair of the sum and a boolean indicating if an
// overflow would occur. If it would, the sum is returned as a wrapped value.
template<std::integral T>
std::pair<T, bool>
overflowing_add(T a, T b)
{
  T ret;
  bool overflow = __builtin_add_overflow(a, b, &ret);
  return {ret, overflow};
}

// overflowing_sub returns a pair of the difference and a boolean indicating if
// an overflow would occur. If it would, the difference is returned as a
// wrapped value.
template<std::integral T>
std::pair<T, bool>
overflowing_sub(T a, T b)
{
  T ret;
  bool overflow = __builtin_sub_overflow(a, b, &ret);
  return {ret, overflow};
}

// overflowing_mul returns a pair of the product and a boolean indicating if an
// overflow would occur. If it would, the product is returned as a wrapped
// value.
template<std::integral T>
std::pair<T, bool>
overflowing_mul(T a, T b)
{
  T ret;
  bool overflow = __builtin_mul_overflow(a, b, &ret);
  return {ret, overflow};
}

// integer_add_overflow adds b to a and stores the result in out. Returns
// true if there was an overflow while performing the addition.
//
// DEPRECATED: use overflowing_add().
template<std::integral T, std::integral U, std::integral Result>
inline bool
integer_add_overflow(T a, U b, Result *out)
{
  return __builtin_add_overflow(a, b, out);
}

// integer_sub_overflow subtracts b from a and stores the result in out. Returns
// true if there was an overflow while performing the subtraction.
template<std::integral T, std::integral U, std::integral Result>
inline bool
integer_sub_overflow(T a, U b, Result *out)
{
  return __builtin_sub_overflow(a, b, out);
}

// integer_add_overflow adds b to a without storing the result.  Returns true if
// there would be an overflow while performing the addition and storing in the
// type Result.  Result defaults to decltype(a+b).
//
// DEPRECATED: use overflowing_add().
template<typename R = void, std::integral T, std::integral U>
inline bool
integer_add_overflow(T a, U b)
{
  using Result = std::conditional_t<std::is_void_v<R>, decltype(a + b), R>;
  // clang doensn't have add_overflow_p but does have add_overflow
#if defined(__builtin_add_overflow_p)
  return __builtin_add_overflow_p(a, b, Result{0});
#else
  Result r;
  return integer_add_overflow(a, b, &r);
#endif
}

// integer_sub_overflow subtracts b from a without storing the result. Returns
// true if there would be an overflow while performing the subtraction and
// storing in the type Result. Result defaults to decltype(a-b).
template<typename R = void, std::integral T, std::integral U>
inline bool
integer_sub_overflow(T a, U b)
{
  using Result = std::conditional_t<std::is_void_v<R>, decltype(a - b), R>;
  // clang doensn't have sub_overflow_p but does have sub_overflow
#if defined(__builtin_sub_overflow_p)
  return __builtin_sub_overflow_p(a, b, Result{0});
#else
  Result r;
  return integer_sub_overflow(a, b, &r);
#endif
}

// kCacheLineSize is the minimum offset between two objects needed to prevent
// false sharing.
#ifdef __cpp_lib_hardware_interference_size
constexpr inline size_t kCacheLineSize =
    std::hardware_destructive_interference_size;
#else  // __cpp_lib_hardware_interference_size
constexpr inline size_t kCacheLineSize = 64;
#endif // __cpp_lib_hardware_interference_size

// kPageSize is the size of a page in bytes.
// TODO(amb): use getpagesize() to confirm size at runtime.
constexpr inline size_t kPageSize = 4096;

} // namespace hogd
