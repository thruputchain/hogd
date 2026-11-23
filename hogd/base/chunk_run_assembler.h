#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "hogd/base/chunk_position.h"
#include "hogd/base/chunked_vector.h"
#include "hogd/base/log.h"

namespace hogd {

// Reassembles a message a sender split into a run of chunks (see
// ChunkPosition) whose chunks may arrive in any order. [Key] identifies the
// message a run carries, such as a block height, and [Part] is one chunk's
// payload; the parts stay separate, moved in as they arrive, so reassembly
// never copies an item, and the run reads them as one sequence. Runs of
// several keys may be pending at once, each completing on its own, and
// push() may be called from any thread: it takes a mutex and never suspends.
// A chunk that breaks its run's framing (a count that changes, an index seen
// twice) is a sender bug and fail-stops.
template<typename Key, typename Part> class ChunkRunAssembler {
public:
  // A reassembled message: its key and its parts in index order, read as one
  // sequence of their items.
  struct Run {
    Key key;
    ChunkedVector<Part> parts;
  };

  // Moves [part] into its slot of the pending run. Returns the run once every
  // chunk of it is in, otherwise nullopt.
  std::optional<Run> push(const Key &key, ChunkPosition position, Part part)
  {
    CHECK_GT(position.count, 0U);
    CHECK_LT(position.index, position.count);
    std::scoped_lock lock(mutex_);
    auto [it, started] = pending_.try_emplace(key);
    Pending &pending = it->second;
    if (started) {
      pending.parts.resize(position.count);
      pending.received.assign(position.count, false);
      pending.remaining = position.count;
    }
    else {
      CHECK_EQ(pending.parts.size(), position.count);
    }
    CHECK(!pending.received[position.index]);
    pending.received[position.index] = true;
    pending.parts[position.index] = std::move(part);
    if (--pending.remaining != 0) {
      return std::nullopt;
    }
    Run run{.key = key, .parts = ChunkedVector<Part>(std::move(pending.parts))};
    pending_.erase(it);
    return run;
  }

private:
  struct Pending {
    std::vector<Part> parts;
    std::vector<bool> received;
    std::uint32_t remaining = 0;
  };
  std::mutex mutex_;
  std::map<Key, Pending> pending_;
};

} // namespace hogd
