#include "hogd/hogexec/components/hash_buffers.h"

#include <cstddef>
#include <span>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/common/status.h"
#include "hogexec_constants.hh"
#include "syscall_error.hh"
#include "types.hh"

namespace hogd {

namespace {

std::vector<std::byte>
make_buffer(std::span<const std::byte> data)
{
  std::vector<std::byte> buffer;
  buffer.reserve(kSHA256BufferMaxSize);
  buffer.insert(buffer.end(), data.begin(), data.end());
  return buffer;
}

void
prepare_buffer_for_reuse(std::vector<std::byte> &buffer)
{
  buffer.clear();
  if (buffer.capacity() < kSHA256BufferMaxSize) {
    buffer.reserve(kSHA256BufferMaxSize);
  }
}

} // namespace

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
    prepare_buffer_for_reuse(it->second);
    return update_existing_buffer(index, data);
  }

  if (buffers_.size() >= kMaxSHA256Buffers) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }

  if (data.size() > kSHA256BufferMaxSize) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }

  buffers_[index] = make_buffer(data);
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
