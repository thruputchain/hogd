#pragma once

#include <cstddef>
#include <span>
#include <unordered_map>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/hog_exec/common/status.h"
#include "types.hh"

namespace hogd {

class HashBuffers {
public:
  SyscallStatus<SHA256Hash> finalize(uint32_t index,
                                     size_t *output_size = nullptr);
  SyscallStatus<void> update(uint32_t index, std::span<const std::byte> data,
                             bool reset);

private:
  SyscallStatus<void> update_existing_buffer(uint32_t index,
                                             std::span<const std::byte> data);

  std::unordered_map<uint32_t, std::vector<std::byte>> buffers_;
};
} // namespace hogd
