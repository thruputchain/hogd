#pragma once

#include <cstdint>

namespace hogd {

// Where one record sits in the run of records that together carry a message
// too large for one RPC record. Every record of a run carries the same count,
// each index in [0, count) is sent exactly once, and a run always has at
// least one record, so an empty message is a run of one empty record. The
// sender may issue a run's calls concurrently and over several connections
// (call_in_chunks); the receiver places each record by index and treats the
// message as complete only once every index has arrived (ChunkRunAssembler),
// so the records of a run may arrive in any order.
struct ChunkPosition {
  std::uint32_t index;
  std::uint32_t count;
};

} // namespace hogd
