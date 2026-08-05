#include "hogd/hogexec/components/hash_buffers.h"

#include <cstddef>
#include <span>
#include <vector>

#include "hog_exec_constants.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/common/status.h"
#include "syscall_error.hh"
#include "types.hh"

namespace hogd {

SyscallStatus<SHA256Hash>
HashBuffers::finalize(uint32_t index, size_t *output_size)
{
  auto it = buffers_.find(index);
  if (it == buffers_.end()) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }
  if (output_size) {
    *output_size = it->second.size();
  }
  auto out = sha256(it->second);
  buffers_.erase(it);
  return out;
}

SyscallStatus<void>
HashBuffers::update(uint32_t index, std::span<const std::byte> data, bool reset)
{
  if (!reset) {
    return update_existing_buffer(index, data);
  }

  auto it = buffers_.find(index);
  if (it != buffers_.end()) {
    if (data.size() > kSHA256BufferMaxSize) {
      return SyscallError{
          SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
    }
    it->second.clear();
    return update_existing_buffer(index, data);
  }

  if (buffers_.size() >= kMaxSHA256Buffers) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }

  if (data.size() > kSHA256BufferMaxSize) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }

  buffers_[index] = std::vector<std::byte>(data.begin(), data.end());
  return {};
}

SyscallStatus<void>
HashBuffers::update_existing_buffer(uint32_t index,
                                    std::span<const std::byte> data)
{
  auto it = buffers_.find(index);
  if (it == buffers_.end()) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }
  if (it->second.size() + data.size() > kSHA256BufferMaxSize) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }
  it->second.insert(it->second.end(), data.begin(), data.end());
  return {};
}

} // namespace hogd
