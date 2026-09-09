// frame_limits.h - the chain's per-frame execution limits.

#pragma once

#include <cstdint>

#include "hogd/base/network_config_params.h"

namespace hogd {

// The configured limits an invocation frame enforces for its whole life,
// fixed at admission from the configuration in force so that a syscall
// enforcing one has no tip read left to fail on.
struct FrameLimits {
  // kConfigParamMaxSHA256Buffers: incremental hash buffers open at once.
  uint32_t max_sha256_buffers;
  // kConfigParamMaxSHA256BufferBytes: bytes one of them may absorb.
  uint32_t max_sha256_buffer_bytes;
  // kConfigParamMaxSavepointDepth: storage savepoints a transaction may hold,
  // one per open TryInvoke frame plus the root commit's.
  uint32_t max_savepoint_depth;

  static constexpr FrameLimits from(NetworkConfigParams const &params)
  {
    return FrameLimits{.max_sha256_buffers = params.max_sha256_buffers,
                       .max_sha256_buffer_bytes =
                           params.max_sha256_buffer_bytes,
                       .max_savepoint_depth = params.max_savepoint_depth};
  }
};

} // namespace hogd
