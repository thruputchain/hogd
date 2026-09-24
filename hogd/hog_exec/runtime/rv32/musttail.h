#pragma once

// Guaranteed tail calls keep interpreter dispatch at constant stack depth.
// Unsupported compilers must fail the build; an empty fallback would grow the
// stack once per instruction. Kept local so the standalone ISA core does not
// depend on hogd/base/compiler.h and its runtime types.
#if defined(__has_attribute) && __has_attribute(musttail)
#define HOGD_MUSTTAIL __attribute__((musttail))
#else
#error                                                                         \
    "rv32 threaded interpreter requires a musttail attribute (clang, or GCC >= 15)"
#endif
