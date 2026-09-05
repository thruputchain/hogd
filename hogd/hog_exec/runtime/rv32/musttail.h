#pragma once

// Guaranteed tail-call attribute for the direct-threaded interpreter. Portable
// spelling: __attribute__((musttail)) works on clang and GCC >= 15.
//
// Deliberately local to the rv32 core rather than in hogd/base/compiler.h: the
// rv32 ISA core is intentionally hogd-free (no hogd/* includes) so it can be
// driven standalone by an external differential oracle. compiler.h pulls in
// hogd/base/types.h, which would break that boundary.
// We #error rather than fall back to an empty macro: the interpreter relies on
// guaranteed tail-call optimization, so a silent degrade to a normal call would
// grow the stack and overflow on long runs. This #error IS the gate -- CMake
// builds the threaded target unconditionally and runs no compiler probe for the
// attribute, so do not "soften" this into an empty fallback macro: that would
// silently convert the guaranteed tail calls into stack-growing calls. A
// toolchain without the attribute fails the build here, by design.
#if defined(__has_attribute) && __has_attribute(musttail)
#define HOGD_MUSTTAIL __attribute__((musttail))
#else
#error                                                                         \
    "rv32 threaded interpreter requires a musttail attribute (clang, or GCC >= 15)"
#endif
