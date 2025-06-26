#pragma once

#include <cstdint>

#include "hogd/test_ffi/test_bindings.h"

namespace hogd {

ForeignHostFnResult mock_syscall_entrypoint(uint64_t syscall, uint64_t arg1,
                                            uint64_t arg2, uint64_t arg3,
                                            uint64_t arg4, uint64_t arg5,
                                            uint64_t arg6,
                                            uint64_t arg7) noexcept;

} // namespace hogd
