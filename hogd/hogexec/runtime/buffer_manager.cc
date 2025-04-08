#include "hogd/hogexec/runtime/buffer_manager.h"

#include <utility>

#include "hogd/base/types.h"

namespace hogd {

std::pair<BufferManager::handle_t, BufferManager::buffer_t &>
BufferManager::allocate_buffer()
{
  uint32_t new_buffer_handle = buffers_.size();
  buffers_.emplace_back();
  return {new_buffer_handle, buffers_.back()};
}

BufferManager::buffer_t *
BufferManager::get_buffer(handle_t handle)
{
  if (handle >= buffers_.size()) {
    return nullptr;
  }
  return &buffers_[handle];
}

} // namespace hogd
