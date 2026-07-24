#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "types.hh" // AddressAndKey, ContractAddress

#include "x23/x23/serialize.h"

// The pure, execution-model-agnostic core of database sharding: the canonical
// key->shard routing, and the cross-shard batch split/gather. Both are
// header-only and dependency-light; the async client facade that drives fan-out
// over a coroutine RPC interface lives separately in sharded_client.h (so
// synchronous consumers such as the executor's DatabaseProxy need not pull in
// the coroutine machinery).

namespace hogd {

// ---------------------------------------------------------------------------
// Routing
// ---------------------------------------------------------------------------

// The primary shard. Global/keyless requests (db tip, viewing snapshots) and
// content-addressed lookups (contracts) route here for now.
inline constexpr size_t kPrimaryShard = 0;

// Canonical key->shard routing for a database sharded into `shard_count`
// shards.
//
// Routes by account address so that all of an address's storage items
// co-locate on a single shard. Deterministic within a process.
//
// NOTE: The hash is a placeholder (FNV-1a over the 32 address bytes). A
// network-agreed function will replace it when sharding becomes externally
// visible; keeping this the only definition makes that swap a single edit.
//
// Precondition: `shard_count >= 1`. Returns a value in [0, shard_count).
inline size_t
shard_of_address(ContractAddress const &address, size_t shard_count)
{
  assert(shard_count >= 1);
  uint64_t h = 14695981039346656037ULL; // FNV-1a offset basis
  for (uint8_t byte : address) {
    h ^= byte;
    h *= 1099511628211ULL; // FNV-1a prime
  }
  return static_cast<size_t>(h % shard_count);
}

// Convenience overload: routes an AddressAndKey by its account address.
inline size_t
shard_of_key(AddressAndKey const &key, size_t shard_count)
{
  return shard_of_address(key.address, shard_count);
}

// ---------------------------------------------------------------------------
// Cross-shard batch split / gather
// ---------------------------------------------------------------------------

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
//
// The split is ZERO-COPY. Rather than materializing per-shard vectors of the
// items (which for a StorageDeltaBatch would malloc and duplicate the entire
// delta footprint -- full bytestrings and all -- only to have the RPC serialize
// a second copy onto the wire), we record just the origin indices and hand each
// shard's RPC a ShardGatherView: a non-owning view that serializes the source
// items[origin[s][j]] in place. This is the cross-shard analogue of
// x23::ProjectedView; the shard's items are non-contiguous in the source, so we
// gather by index instead of spanning a contiguous member projection.

// A non-owning view of the shard-s subset of a source `Batch`. It holds the
// source container plus the indices (origin[s]) routing to shard s, and
// serializes EXACTLY like the Batch the RPC expects -- a length prefix followed
// by each src[origin[s][j]] serialized in place -- without copying the items.
//
// Lifetime: the view is a borrow. The source `Batch` and the index span (owned
// by the ShardSplit) must outlive every use of the view. x23 serializes RPC
// args synchronously at the call site (make_record runs before the first
// co_await), so a view need only survive the call expression; callers keep the
// split and its source alive across the whole fan-out regardless.
template<typename Batch> struct ShardGatherView {
  using value_type = typename Batch::value_type;

  Batch const *src = nullptr;
  std::span<size_t const> indices;

  ShardGatherView() = default;
  ShardGatherView(Batch const &src, std::span<size_t const> indices)
    : src(&src), indices(indices)
  {}

  size_t size() const { return indices.size(); }
  bool empty() const { return indices.empty(); }
};

// x23 custom-serializer hooks for ShardGatherView (found by ADL). These mirror
// the Vec<X> wire format: a uint32 count followed by each element. The byte
// count and the serialized bytes stay in lockstep -- both walk `indices` and
// defer to x23::xdr_size / the serializer for each source element.
template<typename Batch>
size_t
xdr_size(ShardGatherView<Batch> const &v)
{
  size_t ret = 4; // Vec length prefix
  for (size_t idx : v.indices) {
    ret += x23::xdr_size((*v.src)[idx]);
  }
  return ret;
}

template<typename Batch>
void
xdr_serialize(auto &serializer, ShardGatherView<Batch> const &v)
{
  serializer.put32(v.size());
  for (size_t idx : v.indices) {
    serializer((*v.src)[idx]);
  }
}

// The result of splitting a `Batch` (any container with value_type, operator[],
// and range iteration -- std::vector or x23::XdrVec) across `shard_count`
// shards. Holds only the routing map; per-shard sub-batches are produced on
// demand as zero-copy ShardGatherViews via shard_view().
template<typename Batch> struct ShardSplit {
  using value_type = typename Batch::value_type;

  // origin[s][j] is the index into the original input of the j-th item routed
  // to shard s (in input order among that shard). It drives both the per-shard
  // gather view and the positional result reassembly.
  std::vector<std::vector<size_t>> origin;
  // The original input item count.
  size_t total = 0;

  size_t shard_count() const { return origin.size(); }
  bool shard_empty(size_t s) const { return origin[s].empty(); }

  // A zero-copy view of shard s's sub-batch over `items`. `items` must be the
  // same container passed to split_by_shard, and must outlive the view.
  ShardGatherView<Batch> shard_view(Batch const &items, size_t s) const
  {
    return ShardGatherView<Batch>(items, origin[s]);
  }
};

// Split `items` across `shard_count` shards. `route(item, shard_count)` returns
// the owning shard index in [0, shard_count) -- typically shard_of_address /
// shard_of_key above. Records only the routing map; no items are copied.
template<typename Batch, typename Route>
ShardSplit<Batch>
split_by_shard(Batch const &items, size_t shard_count, Route route)
{
  assert(shard_count >= 1);
  ShardSplit<Batch> split;
  split.origin.resize(shard_count);

  size_t i = 0;
  for (auto const &item : items) {
    size_t s = route(item, shard_count);
    assert(s < shard_count);
    split.origin[s].push_back(i);
    ++i;
  }
  split.total = i;
  return split;
}

// Reassemble per-shard results into a single vector in the original input
// order. `per_shard[s]` are the results shard s returned for its sub-batch, in
// the same order (so per_shard[s][j] is the result for the item originally at
// split.origin[s][j]). `ResultBatch` is any container with size() and
// operator[] (std::vector or x23::XdrVec).
//
// Precondition: per_shard has one entry per shard, and each per_shard[s] has
// the same length as split.origin[s]. Returns a vector of length split.total.
template<typename ResultBatch, typename Batch>
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

namespace x23 {
// Declare that a ShardGatherView<Batch> serializes equivalently to the Vec the
// RPC arg expects, provided the source elements serialize like the Vec's
// element type. This is what lets the generated client stub's
// `satisfies_rpc_arg<...>` overload accept the view and forward it, via
// convert_rpc_arg, straight into make_record with no copy.
template<typename X, size_t N, typename Batch>
requires serializes_like<typename hogd::ShardGatherView<Batch>::value_type, X>
struct serialize_equiv<Vec<X, N>, hogd::ShardGatherView<Batch>>
  : std::true_type {};
} // namespace x23
