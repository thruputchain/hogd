// alloc_fault.h - allocation fault injection for tests

#pragma once

// The sanitizer runtimes define the replaceable global allocation functions
// themselves and link them whole-archive, so a second definition fails to link
// (#4130). Under those sanitizers the injector is inert; tests that need it
// check this macro and skip.
#ifdef __has_feature
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#define HOGD_SANITIZER_OWNS_OPERATOR_NEW 1
#endif
#endif
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#define HOGD_SANITIZER_OWNS_OPERATOR_NEW 1
#endif
#ifndef HOGD_SANITIZER_OWNS_OPERATOR_NEW
#define HOGD_SANITIZER_OWNS_OPERATOR_NEW 0
#endif

namespace hogd::test {

// While alive, the Nth allocation on the constructing thread throws
// std::bad_alloc once, aligned allocations included; other threads are
// unaffected. Inert when HOGD_SANITIZER_OWNS_OPERATOR_NEW is set.
class ScopedAllocationFailure {
public:
  explicit ScopedAllocationFailure(int allocations_before_failure);
  ~ScopedAllocationFailure();

  ScopedAllocationFailure(const ScopedAllocationFailure &) = delete;
  ScopedAllocationFailure &operator=(const ScopedAllocationFailure &) = delete;
};

} // namespace hogd::test
