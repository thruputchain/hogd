#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "exec_sched.hh"
#include "hogd/base/coropoll.h"
#include "transaction.hh"
#include "x23/rpc_common.h"
#include "x23/serialize.h"

namespace hogd {

enum class ValidationRpcBatching { Legacy, Bounded64 };

inline constexpr size_t kValidationRpcMaxTransactions = 64;
inline constexpr size_t kValidationRpcBatchTargetBytes = 16 * 1024 * 1024;

using ValidationRpcMethod = ExecutorSched1::proc_meta<std::to_underlying(
    ExecutorSched1::procs::executorsched_validate_batch)>;

inline size_t
validation_rpc_overhead(BlockID block_id)
{
  // x23's payload cap excludes the four-byte ONC record marker. The
  // variable-length argument carries a four-byte vector count.
  return x23::xdr_size(x23::call_msg<ValidationRpcMethod>()) +
         x23::xdr_size(block_id) + x23::xdr_size(uint32_t{});
}

inline std::vector<ChunkRange>
plan_validation_rpc_batches(BlockID block_id,
                            x23::span<const ProposedTransaction> txs,
                            size_t target_bytes, size_t record_cap)
{
  const size_t overhead = validation_rpc_overhead(block_id);
  CHECK_LE(overhead, target_bytes);
  CHECK_LE(target_bytes, record_cap);
  const size_t target = target_bytes - overhead;
  const size_t cap = record_cap - overhead;

  const auto balanced =
      make_chunks_for_max_size(txs.size(), kValidationRpcMaxTransactions);
  std::vector<ChunkRange> chunks;
  chunks.reserve(balanced.size());
  for (auto [start, end] : balanced) {
    size_t bytes = 0;
    for (size_t i = start; i < end; ++i) {
      const size_t tx_bytes = x23::xdr_size(txs[i]);
      if (tx_bytes > cap) {
        throw x23::XdrBadInput("executor validation transaction exceeds RPC "
                               "record cap: {} bytes plus {} overhead, cap {}",
                               tx_bytes, overhead, record_cap);
      }
      // Retain a sendable large singleton. Splitting protects the normal
      // batching budget without changing the accepted transaction envelope.
      if (i != start && tx_bytes > target - bytes) {
        chunks.push_back({.start = start, .end = i});
        start = i;
        bytes = 0;
      }
      if (tx_bytes > target) {
        chunks.push_back({.start = i, .end = i + 1});
        start = i + 1;
      }
      else {
        bytes += tx_bytes;
      }
    }
    if (start != end) {
      chunks.push_back({.start = start, .end = end});
    }
  }
  return chunks;
}

} // namespace hogd
