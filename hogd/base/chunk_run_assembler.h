#pragma once

#include <cstdint>
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
// never copies an item, and the run reads them as one sequence. At most one
// run is pending, because a sender starts its next run only after every chunk
// of this one is acknowledged; the caller serializes push(). A chunk that
// breaks its run's framing (another key while a run is pending, a count that
// changes, an index seen twice) is a sender bug and fail-stops.
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
    if (!pending_.has_value()) {
      Pending &started = pending_.emplace();
      started.key = key;
      started.parts.resize(position.count);
      started.received.assign(position.count, false);
      started.remaining = position.count;
    }
    else {
      CHECK(pending_->key == key);
      CHECK_EQ(pending_->parts.size(), position.count);
    }
    Pending &pending = *pending_;
    CHECK(!pending.received[position.index]);
    pending.received[position.index] = true;
    pending.parts[position.index] = std::move(part);
    if (--pending.remaining != 0) {
      return std::nullopt;
    }
    Run run{.key = std::move(pending.key),
            .parts = ChunkedVector<Part>(std::move(pending.parts))};
    pending_.reset();
    return run;
  }

private:
  struct Pending {
    Key key;
    std::vector<Part> parts;
    std::vector<bool> received;
    std::uint32_t remaining = 0;
  };
  std::optional<Pending> pending_;
};

} // namespace hogd
