#pragma once

#include <cstdint>

#include "hogd/test_ffi/test_bindings.h"
namespace hogd {

ForeignHostFnResult mock_syscall_entrypoint(int32_t syscall,
                                            const unsigned char *input,
                                            uint32_t input_len,
                                            unsigned char *output,
                                            uint32_t output_len) noexcept;

} // namespace hogd
