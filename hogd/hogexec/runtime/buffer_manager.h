#pragma once

#include <cstddef>
#include <utility>
#include <vector>

#include "hogd/base/types.h"

namespace hogd {

// Maintain a set of indexed buffers.
// Contracts can open new buffers and append to buffers,
// which are identified by handles (u32).
// These buffers and handles are not passed from one contract
// to another.
//
// This is implemented for contract convenience and simplicity (this logic
// could, of course, be implemented within a contract).
//
// The existing set of syscalls will, as necessary, be augmented to support
// taking these handles as inputs, in lieu of span arguments.
class BufferManager {
public:
  using handle_t = uint32_t;
  using buffer_t = std::vector<std::byte>;

  std::pair<handle_t, buffer_t &> allocate_buffer();
  // returns nullptr if handle is not an allocated handle
  buffer_t *get_buffer(handle_t handle);

private:
  std::vector<buffer_t> buffers_;
};

} // namespace hogd
