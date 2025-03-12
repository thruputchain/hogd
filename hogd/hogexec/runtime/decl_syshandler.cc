#include "hogd/hogexec/runtime/decl_syshandler.h"

#include <concepts>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_entry.h"
#include "lfi_small.h"

namespace hogd {

uint64_t decl_syshandler(void *call_context, uint64_t arg0,
                           uint64_t arg1, uint64_t arg2, uint64_t arg3,
                           uint64_t arg4, uint64_t arg5, uint64_t arg6) noexcept
{
    HostFnResult res;
    try {
        res = syscall_entry(reinterpret_cast<HostCallContext*>(call_context), arg0, arg1, arg2, arg3, arg4, arg5, arg6);
    } catch (...) { // NOLINT(bugprone-empty-catch)
        res.first = HostFnErrorCode::kUnrecoverable;
        res.second = 0;
    }
    // guard against future refactors
    static_assert(std::same_as<decltype(std::to_underlying(res.first)), int32_t>);
    static_assert(std::same_as<decltype(res.second), int32_t>);

    // convert to unsigned before shifting
    uint32_t host_fn_error_bits = std::to_underlying(res.first);
    uint64_t host_fn_bits_shifted = uint64_t{host_fn_error_bits} << 32;
    uint32_t syscall_error = res.second;

    // The lowbits are interpreted as things returned from the lfi proc,
    // while the highbits are those returned by the syscall.  
    // Never returns both.
    switch(res.first) {
        case HostFnErrorCode::kNoneOrRecoverable:
            return syscall_error;
        case HostFnErrorCode::kReturn:
            lfi_proc_exit(0);
        case HostFnErrorCode::kExit:
            lfi_proc_exit(syscall_error);
        case HostFnErrorCode::kOutOfGas:
        case HostFnErrorCode::kUnrecoverable:
            lfi_proc_exit(host_fn_bits_shifted);
    }
    std::unreachable();
}



}