#pragma once

#include <cstddef>
#include <span>
#include <unordered_map>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/crypto/crypto.h"
#include "hogd/hog_exec/common/status.h"
#include "types.hh"

namespace hogd {

class HashBuffers {
public:
  // `max_buffers` is the chain's kConfigParamMaxSHA256Buffers.
  explicit HashBuffers(uint32_t max_buffers) : max_buffers_(max_buffers) {}

  [[nodiscard]] SyscallStatus<SHA256Hash> finalize(uint32_t index);
  [[nodiscard]] SyscallStatus<void>
  update(uint32_t index, std::span<const std::byte> data, bool reset);
  [[nodiscard]] SyscallStatus<void> copy(uint32_t src_index,
                                         uint32_t dst_index);

private:
  // One incremental hash in progress.
  struct Stream {
    SHA256Hasher hasher;
  };

  [[nodiscard]] SyscallStatus<void>
  update_existing_stream(uint32_t index, std::span<const std::byte> data);

  // Contract-chosen indices are attacker-controlled, so the table hashes them
  // with the process-keyed SipHash24 (like the codebase's other attacker-keyed
  // maps): the key is secret, so collisions can't be precomputed to degrade a
  // metered lookup, and buckets stay balanced in O(1) average.
  std::unordered_map<uint32_t, Stream, SipHash24Hasher> streams_;

  const uint32_t max_buffers_;
};
} // namespace hogd
