// compiler.h - low-level compiler utilities

#pragma once

namespace hogd {

// CSTR - converts a preprocessor value to a string
#define CSTR_T(x...) #x
#define CSTR(x...) CSTR_T(x)

// variable attributes
#define ATTRIBUTE_PACKED __attribute__((packed))

// function attributes
#define ATTRIBUTE_NOINLINE __attribute__((noinline))

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

} // namespace hogd
