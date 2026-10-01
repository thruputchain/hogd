#pragma once

#include <cstddef>
#include <map>
#include <span>

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

  // Contract input chooses these stream indices, so the table must not let
  // crafted keys amplify per-operation cost. A std::map keeps find/emplace/
  // erase at O(log n) worst case, bounded by max_buffers_, whatever the key
  // distribution; a hashed table lets colliding indices degrade a metered
  // lookup to O(n).
  std::map<uint32_t, Stream> streams_;

  const uint32_t max_buffers_;
};
} // namespace hogd
