#pragma once

#include <array>

#include "hogd/base/types.h"
#include "syscall.hh"

namespace hogd {

// Track additional stats of resource usage within a transaction.
// Will be optionally enabled during benchmarks (for measuring gas costs of
// various operations) but disabled during regular operation.
struct TransactionStats {

  TransactionStats() = default;

  // disable copy and move
  TransactionStats(TransactionStats &&) = delete;
  TransactionStats(TransactionStats const &) = delete;
  TransactionStats &operator=(TransactionStats &&) = delete;
  TransactionStats &operator=(TransactionStats const &) = delete;

  void print_stats() const;

  std::array<uint16_t, kSyscallCount> syscall_counts = {};
  uint16_t syscall_fault_count = 0;
  uint16_t invalid_syscall_count = 0;

  // per-syscall related metrics
  size_t sha256_total_bytes_processed = 0;
};

} // namespace hogd
