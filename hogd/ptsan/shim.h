// shim.h - calling code PointerTagSanitizer did not instrument
//
// Used only when HOGD_PTSAN is on; see docs/ptsan.md.
//
// PTSan keeps an object ID in the upper bits of every pointer instrumented
// code creates, and instrumented code removes it before each access. A
// library the PTSan compiler did not build needs two things at every call:
//
//  - raw addresses. A tagged pointer is not a canonical address, and the
//    library's first access through one faults.
//  - untagged allocations. PTSan's malloc replaces libc's for the whole
//    process, so memory the library allocates would come back tagged as well.
//    ptsan_in_libc is the runtime's per-thread count of calls into
//    uninstrumented code; while it is nonzero the allocator returns plain
//    pointers.
//
// There are two ways to put a shim in front of a function f:
//
//  - Wrapping, for libraries only hogd calls. The shim file names f with
//    HOGD_PTSAN_WRAPPED(f) and defines __wrap_f, which forwards to __real_f.
//    Every user of the library then links with -Wl,--wrap=f, so that calls to
//    f land on __wrap_f and __real_f is the library's own f.
//  - Interposing, for libc, which the PTSan libc++ calls as well, from its
//    own shared object. The shim file names f with HOGD_PTSAN_INTERPOSED(f),
//    defines f itself, and forwards to HOGD_PTSAN_NEXT(f), the definition the
//    dynamic linker would otherwise have used. Every executable then links
//    with -Wl,--undefined=f, which pulls in the shim even when only libc++
//    calls f.
//
// hogd_ptsan_shim_library() in cmake/ptsan.cmake builds a shim file and takes
// those linker options from its HOGD_PTSAN_WRAPPED and HOGD_PTSAN_INTERPOSED
// lines.
//
// Shim files are compiled without PTSan (-fno-sanitize=ptsan). They receive
// tagged pointers and must apply raw() before any access through one.
//
// A function with no shim still links. At run time it either faults inside
// the library or, for a system call, fails with EFAULT. Every function hogd
// calls in an uninstrumented library needs a shim, including functions that
// take no pointer but may allocate.

#pragma once

#if __has_feature(pointer_tag_sanitizer)
#error "PTSan shims must be compiled with -fno-sanitize=ptsan"
#endif

#include <dlfcn.h>

#include <cstdint>
#include <type_traits>

extern "C" {
// PTSan runtime interface (compiler-rt/lib/ptsan).
extern __thread uint32_t ptsan_in_libc;
uint64_t ptsan_get_address(const void *ptr);
}

namespace hogd::ptsan {

// Counts this thread into uninstrumented code for one call.
class UninstrumentedCall {
public:
  UninstrumentedCall() noexcept { ++ptsan_in_libc; }
  ~UninstrumentedCall() { --ptsan_in_libc; }
  UninstrumentedCall(const UninstrumentedCall &) = delete;
  UninstrumentedCall &operator=(const UninstrumentedCall &) = delete;
};

// raw returns the address a possibly tagged pointer refers to.
template<typename T>
T *
raw(T *p) noexcept
{
  return reinterpret_cast<T *>(ptsan_get_address(p));
}

} // namespace hogd::ptsan

// Both of these go one per line, at the start of the line: CMake greps for
// them.
//
// HOGD_PTSAN_WRAPPED(f) declares __real_f and __wrap_f with f's own type, so
// a signature that drifts from the library's fails to compile instead of
// passing mismatched arguments.
#define HOGD_PTSAN_WRAPPED(f)                                                  \
  extern "C" decltype(f) __real_##f;                                           \
  extern "C" decltype(f) __wrap_##f

// HOGD_PTSAN_INTERPOSED(f) marks f as a libc function this file defines.
#define HOGD_PTSAN_INTERPOSED(f) static_assert(std::is_function_v<decltype(f)>)

// The definition of f that the shim defining f hides.
#define HOGD_PTSAN_NEXT(f)                                                     \
  ([] {                                                                        \
    static const auto next =                                                   \
        reinterpret_cast<decltype(f) *>(dlsym(RTLD_NEXT, #f));                 \
    return next;                                                               \
  }())
