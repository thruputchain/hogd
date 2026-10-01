#include "hogd/hog_exec/components/hash_buffers.h"

#include <cstddef>
#include <span>

#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_exec/common/status.h"
#include "syscall_error.hh"
#include "types.hh"

namespace hogd {

SyscallStatus<SHA256Hash>
HashBuffers::finalize(uint32_t index)
{
  auto it = streams_.find(index);
  if (it == streams_.end()) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }
  auto out = it->second.hasher.finalize();
  streams_.erase(it);
  return out;
}

SyscallStatus<void>
HashBuffers::update(uint32_t index, std::span<const std::byte> data, bool reset)
{
  if (!reset) {
    return update_existing_stream(index, data);
  }

  auto it = streams_.find(index);
  if (it == streams_.end()) {
    if (streams_.size() >= max_buffers_) {
      return SyscallError{
          SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
    }
    it = streams_.emplace(index, Stream{}).first;
  }
  else {
    it->second = Stream{};
  }
  it->second.hasher.update(data);
  return {};
}

SyscallStatus<void>
HashBuffers::copy(uint32_t src_index, uint32_t dst_index)
{
  auto src = streams_.find(src_index);
  if (src == streams_.end()) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }
  if (src_index == dst_index) {
    return {};
  }
  auto dst = streams_.find(dst_index);
  if (dst == streams_.end()) {
    if (streams_.size() >= max_buffers_) {
      return SyscallError{
          SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
    }
    streams_.emplace(dst_index, src->second);
  }
  else {
    dst->second = src->second;
  }
  return {};
}

SyscallStatus<void>
HashBuffers::update_existing_stream(uint32_t index,
                                    std::span<const std::byte> data)
{
  auto it = streams_.find(index);
  if (it == streams_.end()) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }
  it->second.hasher.update(data);
  return {};
}

} // namespace hogd
