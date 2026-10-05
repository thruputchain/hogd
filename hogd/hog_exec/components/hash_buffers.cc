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

  // Each lookup costs a keyed SipHash24 of the index, so hash it once: below
  // the cap try_emplace both finds and inserts; at the cap only an existing
  // slot can be reset.
  Stream *stream = nullptr;
  if (streams_.size() < max_buffers_) {
    auto [it, inserted] = streams_.try_emplace(index);
    stream = &it->second;
    if (!inserted) {
      *stream = Stream{};
    }
  }
  else {
    auto it = streams_.find(index);
    if (it == streams_.end()) {
      return SyscallError{
          SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
    }
    stream = &it->second;
    *stream = Stream{};
  }
  stream->hasher.update(data);
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
  // Hash dst_index once, as in update(). A rehash inside try_emplace
  // invalidates iterators but not references to elements, so the copy source
  // stays valid while the new node is constructed from it.
  const Stream &src_stream = src->second;
  if (streams_.size() < max_buffers_) {
    auto [dst, inserted] = streams_.try_emplace(dst_index, src_stream);
    if (!inserted) {
      dst->second = src_stream;
    }
    return {};
  }
  auto dst = streams_.find(dst_index);
  if (dst == streams_.end()) {
    return SyscallError{SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput};
  }
  dst->second = src_stream;
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
