#pragma once

#include <cstddef>
#include <cstdint>
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
  SyscallStatus<SHA256Hash> finalize(uint32_t index,
                                     size_t *output_size = nullptr);
  // `max_buffers` is the chain's kConfigParamMaxSHA256Buffers. It is passed in
  // rather than held per frame: the value is fixed for a block, and every
  // frame in a block shares it.
  SyscallStatus<void> update(uint32_t index, std::span<const std::byte> data,
                             bool reset, uint32_t max_buffers);
  SyscallStatus<void> copy(uint32_t src_index, uint32_t dst_index,
                           uint32_t max_buffers);

private:
  // One incremental hash in progress: a streaming SHA-256 midstate plus the
  // byte count absorbed since the last reset. The state is constant-size
  // regardless of how many bytes were fed, so copying a stream (a planned
  // fork operation for keypath derivation) is constant-time. The byte count
  // enforces kSHA256BufferMaxSize -- contract-observable, so it outlives the
  // representation that needed it — and feeds finalize's size report into
  // transaction stats.
  struct Stream {
    SHA256Hasher hasher;
    size_t size = 0;
  };

  SyscallStatus<void> update_existing_stream(uint32_t index,
                                             std::span<const std::byte> data);

  std::unordered_map<uint32_t, Stream> streams_;
};
} // namespace hogd
