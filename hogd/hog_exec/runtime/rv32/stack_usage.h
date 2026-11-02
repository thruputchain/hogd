#pragma once

#include <cstdint>
#include <limits>

// Debug-only guest stack-usage diagnostic, shared by the threaded interpreter
// (which records it), Rv32Runtime (which hands it to the syscall handler) and
// the test FFI (which reports it). No protocol or executor dependencies: the
// ISA core includes this too.
//
// CMake defines HOGD_RV32_STACK_USAGE (option of the same name, on by default
// with ENABLE_TEST_FFI) for Debug configurations only; the NDEBUG check keeps
// a stray definition out of an optimized interpreter, whose handlers must not
// carry the per-write compare. Every translation unit must see the same value:
// the diagnostic adds members to ThreadedMachine and SystemCallHandler, so a
// per-target definition would be an ODR violation. That is why CMake sets it
// globally.
#if defined(HOGD_RV32_STACK_USAGE) && !defined(NDEBUG)
#define HOGD_RV32_STACK_USAGE_ENABLED 1
#else
#define HOGD_RV32_STACK_USAGE_ENABLED 0
#endif

namespace hogd::rv32 {

// x2 is sp in the RISC-V calling convention; the guest's _start establishes it
// (entry ABI v3 seeds it with zero) and the stack grows down from there.
inline constexpr uint8_t kStackPointerReg = 2;

// The recorded initial value and minimum until the guest first writes sp. No
// real stack pointer takes the value: sp addresses guest memory, which is
// bounded by kMemoryCeilingBytes.
inline constexpr uint32_t kSpUnwritten = std::numeric_limits<uint32_t>::max();

} // namespace hogd::rv32
