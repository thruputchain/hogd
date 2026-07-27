#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "types.hh" // AddressAndKey, ContractAddress

#include "hogd/base/log.h" // CHECK_EQ
#include "x23/x23/serialize.h"

// The execution-model-agnostic core of database sharding: key->shard routing
// and the cross-shard batch split/gather. Header-only; the async fan-out facade
// lives separately in sharded_client.h so synchronous consumers need not pull
// in the coroutine machinery.

namespace hogd {

// The db sharding model. State is placed one of four ways; every RPC must read
// the way its writes routed (a mismatch reads the wrong shard -- the
// get_deployment bug). Classify every new RPC against this.
//
//   1. Keyed (storage items, deployments, balances, RCQ, hog ages): owner
//      shard, shard_of_address(address). By address, not full key, so an
//      account's items co-locate; addresses are sha256-derived, so never route
//      by user-chosen bytes.
//   2. Content-addressed (contract code): primary shard (kPrimaryShard) only,
//      by hash. Interim -- code is immutable, so hash % N is also safe.
//   3. Replicated control (block lifecycle, genesis): written to every shard;
//      each keeps its own chain and validates create_snapshot(prev) against it,
//      so shards advance in lockstep. Reads require agreement on
//      (height, id, block_hash), NOT db_hash (legitimately per-shard).
//   4. Per-shard-minted (viewing snapshot ids): each shard mints its own, so a
//      cross-shard op holds one per shard. NOT yet wired -- DatabaseProxy opens
//      on the primary alone (tracked separately).

// ---------------------------------------------------------------------------
// Routing
// ---------------------------------------------------------------------------

// The primary shard: content-addressed lookups (class 2) and global/keyless
// requests (db tip) route here. See the sharding model above.
inline constexpr size_t kPrimaryShard = 0;

// Routes by account address so all of an address's storage items co-locate on
// one shard. Deterministic within a process. Precondition: `shard_count >= 1`.
// Returns a value in [0, shard_count).
//
// The hash is a placeholder (FNV-1a) until a network-agreed function replaces
// it; this is its only definition. KNOWN LIMITATION (low priority): FNV is
// unkeyed, so an adversary who grinds addresses can bias the distribution and
// overload one shard. The intended fix is a keyed hash (SipHash-2-4 is already
// available), but that needs a validator-recoverable key so the key->shard
// mapping survives restart; until we solve key recovery this stays FNV. Not
// worth bug reports in the meantime.
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

// Splits a keyed batch per-shard and reassembles results into the original
// input order. Zero-copy: it records only per-shard indices and serializes
// source items in place via ShardGatherView, never duplicating the batch.

// A non-owning view of shard s's subset of a source `Batch` (the source plus
// its origin[s] indices) that serializes like the Batch the RPC expects without
// copying. A borrow: the source and index span must outlive it (they live in
// the ShardSplit, and x23 serializes args before the first co_await anyway).
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

// x23 custom-serializer hooks (found by ADL), mirroring the Vec<X> wire format:
// a uint32 count then each element. xdr_size and xdr_serialize must stay in
// lockstep.
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

// The routing map from splitting a `Batch` across `shard_count` shards. Holds
// only indices plus a borrow of the source batch; per-shard sub-batches are
// produced on demand as zero-copy ShardGatherViews via shard_view().
//
// `src` borrows the exact container passed to split_by_shard, which must
// outlive the split (and the split must outlive any views / awaits). Binding it
// here rather than re-passing it to shard_view means the indices can only ever
// be resolved against the batch they were computed from -- there is no way to
// hand shard_view the wrong container or one resized since the split.
template<typename Batch> struct ShardSplit {
  using value_type = typename Batch::value_type;

  // origin[s][j] is the input index of the j-th item routed to shard s, in
  // input order. Drives both shard_view and positional reassembly.
  std::vector<std::vector<size_t>> origin;
  Batch const *src = nullptr; // the batch these indices index into
  size_t total = 0;           // original input item count

  size_t shard_count() const { return origin.size(); }
  bool shard_empty(size_t s) const { return origin[s].empty(); }

  // Zero-copy view of shard s's sub-batch over the bound source batch.
  ShardGatherView<Batch> shard_view(size_t s) const
  {
    assert(src != nullptr);
    return ShardGatherView<Batch>(*src, origin[s]);
  }
};

// Split `items` across `shard_count` shards. `route(item, shard_count)` returns
// the owning shard index (typically shard_of_address/shard_of_key). Records
// only the routing map and a borrow of `items` (which must outlive the split);
// no items are copied.
template<typename Batch, typename Route>
ShardSplit<Batch>
split_by_shard(Batch const &items, size_t shard_count, Route route)
{
  assert(shard_count >= 1);
  ShardSplit<Batch> split;
  split.origin.resize(shard_count);
  split.src = &items;

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

// Reassemble per-shard results into input order: per_shard[s][j] is the result
// for the item at split.origin[s][j]. per_shard has one entry per shard (an
// internal invariant of the caller). Returns split.total results.
template<typename ResultBatch, typename Batch>
std::vector<typename ResultBatch::value_type>
gather_positional(ShardSplit<Batch> const &split,
                  std::vector<ResultBatch> const &per_shard)
{
  using Result = typename ResultBatch::value_type;
  assert(per_shard.size() == split.shard_count());

  std::vector<Result> out(split.total);
  for (size_t s = 0; s < split.shard_count(); ++s) {
    // A shard must return exactly one result per item we routed to it. This is
    // shard-controlled (per_shard[s] is the shard's RPC reply): a too-long
    // reply would index past origin[s] below -- an OOB read/write on the
    // critical path -- and there is no error channel out of here. A shard
    // answering a count we never asked for is broken; stop rather than march on
    // against it (same call as the db-tip divergence check).
    CHECK_EQ(per_shard[s].size(), split.origin[s].size());
    for (size_t j = 0; j < per_shard[s].size(); ++j) {
      out[split.origin[s][j]] = per_shard[s][j];
    }
  }
  return out;
}

} // namespace hogd

namespace x23 {
// A ShardGatherView serializes like the Vec the RPC expects (given matching
// element types), so the generated stub's satisfies_rpc_arg<...> overload
// accepts and forwards it with no copy.
template<typename X, size_t N, typename Batch>
requires serializes_like<typename hogd::ShardGatherView<Batch>::value_type, X>
struct serialize_equiv<Vec<X, N>, hogd::ShardGatherView<Batch>>
  : std::true_type {};
} // namespace x23
