#pragma once

#include <cassert>
#include <cstddef>
#include <vector>

namespace hogd {

// The pure, execution-model-agnostic core of cross-shard batch handling.
//
// A keyed batch (e.g. hog-ages by account, or storage deltas by addr+key) is
// split into per-shard sub-batches, each sub-batch is sent to its shard by the
// caller's own fan-out driver (a synchronous loop on the executor side, a
// coroutine loop on the scheduler side), and per-shard results are reassembled
// into the ORIGINAL input order.
//
// The order-preservation is the correctness-critical piece: callers such as
// Scheduler::read_average_hog_ages rely on results[i] corresponding to
// accounts[i]. Keeping that logic here -- tested once -- keeps it out of every
// per-interface fan-out driver.

// The result of splitting a `Batch` (any container with value_type, push_back,
// and range iteration -- std::vector or x23::XdrVec) across `shard_count`
// shards.
template <typename Batch>
struct ShardSplit {
  using value_type = typename Batch::value_type;

  // batches[s] holds the items routed to shard s, in input order among that
  // shard. Ready to hand directly to the shard's RPC.
  std::vector<Batch> batches;
  // origin[s][j] is the index into the original input of batches[s][j] --
  // the map used to reassemble results positionally.
  std::vector<std::vector<size_t>> origin;
  // The original input item count.
  size_t total = 0;

  size_t shard_count() const { return batches.size(); }
  bool shard_empty(size_t s) const { return batches[s].empty(); }
};

// Split `items` across `shard_count` shards. `route(item, shard_count)` returns
// the owning shard index in [0, shard_count) -- typically a call into the
// shared shard_router (shard_of_address / shard_of_key).
template <typename Batch, typename Route>
ShardSplit<Batch> split_by_shard(Batch const &items, size_t shard_count,
                                 Route route)
{
  assert(shard_count >= 1);
  ShardSplit<Batch> split;
  split.batches.resize(shard_count);
  split.origin.resize(shard_count);

  size_t i = 0;
  for (auto const &item : items) {
    size_t s = route(item, shard_count);
    assert(s < shard_count);
    split.batches[s].push_back(item);
    split.origin[s].push_back(i);
    ++i;
  }
  split.total = i;
  return split;
}

// Reassemble per-shard results into a single vector in the original input
// order. `per_shard[s]` are the results shard s returned for split.batches[s],
// in the same order (so per_shard[s][j] is the result for the item originally
// at split.origin[s][j]). `ResultBatch` is any container with size() and
// operator[] (std::vector or x23::XdrVec).
//
// Precondition: per_shard has one entry per shard, and each per_shard[s] has
// the same length as split.batches[s]. Returns a vector of length split.total.
template <typename ResultBatch, typename Batch>
std::vector<typename ResultBatch::value_type>
gather_positional(ShardSplit<Batch> const &split,
                  std::vector<ResultBatch> const &per_shard)
{
  using Result = typename ResultBatch::value_type;
  assert(per_shard.size() == split.shard_count());

  std::vector<Result> out(split.total);
  for (size_t s = 0; s < split.shard_count(); ++s) {
    assert(per_shard[s].size() == split.origin[s].size());
    for (size_t j = 0; j < per_shard[s].size(); ++j) {
      out[split.origin[s][j]] = per_shard[s][j];
    }
  }
  return out;
}

} // namespace hogd
