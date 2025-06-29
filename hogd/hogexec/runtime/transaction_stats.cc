#include "hogd/hogexec/runtime/transaction_stats.h"

#include <print>

#include "hogd/base/types.h"
#include "syscall.hh"

namespace hogd {


void TransactionStats::print_stats() const {

    for (size_t i = 0; i < kSyscallCount; i++) {
        std::println("Syscall {} count {}", SyscallNo(i), syscall_counts[i]);
    }

    std::println("Syscall fault count: {}", syscall_fault_count);
    std::println("Invalid SyscallNo count: {}", invalid_syscall_count);
    std::println("SHA256 bytes processed: {}", sha256_total_bytes_processed);
}


}