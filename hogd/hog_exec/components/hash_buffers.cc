#include "hogd/hog_exec/components/hash_buffers.h"

#include <cstddef>
#include <span>

#include "hog_exec_constants.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_exec/common/status.h"
#include "syscall_error.hh"
#include "types.hh"

namespace hogd {

SyscallStatus<SHA256Hash>
HashBuffers::finalize(uint32_t index, size_t *output_size)
{
  auto it = streams_.find(index);
  if (it == streams_.end()) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }
  if (output_size) {
    *output_size = it->second.size;
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

  // Validate before touching any state: a rejected reset must leave an
  // existing stream hashing as if the call never happened.
  if (data.size() > kSHA256BufferMaxSize) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }

  auto it = streams_.find(index);
  if (it == streams_.end()) {
    if (streams_.size() >= kMaxSHA256Buffers) {
      return SyscallError{
          SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
    }
    it = streams_.emplace(index, Stream{}).first;
  }
  else {
    it->second = Stream{};
  }
  it->second.hasher.update(data);
  it->second.size = data.size();
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
  // Reject before absorbing: a failed append must leave the stream hashing
  // as if the call never happened.
  if (it->second.size + data.size() > kSHA256BufferMaxSize) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }
  it->second.hasher.update(data);
  it->second.size += data.size();
  return {};
}

} // namespace hogd
