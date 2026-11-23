// -*-C++-*-

#pragma once

#include <cstdio>
#include <sys/resource.h>
#include <unistd.h>

#include <gtest/gtest.h>

namespace x23::test {

inline void
disable_core_dumps()
{
  rlimit limit;
  if (getrlimit(RLIMIT_CORE, &limit) == 0) {
    limit.rlim_cur = 0;
    if (setrlimit(RLIMIT_CORE, &limit) == 0)
      return;
  }
  std::perror("death test: cannot disable core dumps");
  // A successful exit fails EXPECT_DEATH rather than accidentally satisfying
  // its expected crash when configuring the child fails.
  _exit(0);
}

} // namespace x23::test

// Run the limit change inside Google's death-test statement, hence only in
// the child in both fast and threadsafe modes. Unexpected crashes in the
// parent and ordinary tests retain their core dumps. This does not disable
// macOS CrashReporter, which is separate from RLIMIT_CORE.
#define X23_EXPECT_DEATH(statement, matcher)                                   \
  EXPECT_DEATH(                                                                \
      {                                                                        \
        ::x23::test::disable_core_dumps();                                     \
        statement;                                                             \
      },                                                                       \
      matcher)

#define X23_ASSERT_DEATH(statement, matcher)                                   \
  ASSERT_DEATH(                                                                \
      {                                                                        \
        ::x23::test::disable_core_dumps();                                     \
        statement;                                                             \
      },                                                                       \
      matcher)
