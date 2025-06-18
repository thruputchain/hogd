// macros for checking conditions.

// TODO: Add open source license?

#pragma once

#include <thread>

#define __LCHECK(op, invop, expr1, expr2)                                      \
  do {                                                                         \
    auto __val1 = expr1;                                                       \
    auto __val2 = expr2;                                                       \
    if (!(__val1 op __val2)) {                                                 \
      int __errno = errno;                                                     \
      std::cerr << __FILE__ << ":" << __LINE__ << "("                          \
                << std::this_thread::get_id() << ") "                          \
                << "CHECK FAILED: " << #expr1 << " " #op " " << #expr2 << " [" \
                << __val1 << " " #invop " " << __val2 << "]" << std::endl;     \
      if (__errno) {                                                           \
        std::cerr << "errno: " << __errno << " [" << std::strerror(__errno)    \
                  << "]" << std::endl;                                         \
      }                                                                        \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

#define CHECK_EQ(val1, val2) __LCHECK(==, !=, val1, val2)
#define CHECK_NE(val1, val2) __LCHECK(!=, ==, val1, val2)
#define CHECK_LT(val1, val2) __LCHECK(<, >=, val1, val2)
#define CHECK_LE(val1, val2) __LCHECK(<=, <, val1, val2)
#define CHECK_GT(val1, val2) __LCHECK(>, <=, val1, val2)
#define CHECK_GE(val1, val2) __LCHECK(>=, >, val1, val2)
#define CHECK_PTR(val1) CHECK_NE(val1, nullptr)
#define CHECK(val1) CHECK_NE(val1, 0)
