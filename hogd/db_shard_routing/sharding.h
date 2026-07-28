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

// Which hogdb holds a given key.
//
// A ramdb key is a one-byte prefix (hogdb_dao.h Prefix) followed by 32-byte
// fields:
//
//   'B' | address              native token balance       hashed
//   'D' | address              auth data                  hashed
//   'K' | address | key        contract data              hashed
//   'Q' | address | key        retrocausal queue block    hashed
//   'C' | code hash            wasm code                  hashed
//   'A' | address              hog age                    not hashed
//   '<' ...                    db tip, cost model         not hashed
//
// An address is the sha256 of a public key (accounts) or of a deployment
// preimage (contracts), so addresses spread uniformly on their own.
//
// shard_of_address() picks the shard from the address field alone. Everything
// one account owns therefore lands on one shard, which is why the key field is
// left out of the choice. A read has to repeat the routing its write used, or
// it asks a shard that does not hold the item.
//
// 'C' keys hold a content hash where the others hold an address, so code lives
// on shard 0 for now. Code never changes after it is written, so routing on the
// code hash would work as well.
//
// '<' keys are not sharded. Each shard keeps its own globals and runs the whole
// block lifecycle, validating create_snapshot(prev) against its own chain, so
// shard tips must agree on height, id, and block_hash.
//
// db_hash is the one field that differs, because a shard hashes only the items
// it holds (is_state_item in hogdb_dao.h decides which prefixes count). Those
// hashes are LtHash16 multiset values, so the shards' sum to the hash an
// unsharded db would produce, as long as each item is hashed on exactly one
// shard. Genesis breaks that today by writing every item to every shard, and
// the block header carries shard 0's hash alone.

// ---------------------------------------------------------------------------
// Routing
// ---------------------------------------------------------------------------

// Shard 0. Holds the wasm code, and answers the requests that carry no address
// (db tip, viewing snapshots).
inline constexpr size_t kPrimaryShard = 0;

// Identifies shard_of_address below. Persisted per shard and declared in its
// ShardCard; a mismatch is fail-stop (changing the function once a cluster has
// data would misroute lookups). Bump when shard_of_address changes.
inline constexpr uint32_t kShardFunctionId = 0;

// Deterministic within a process. Precondition: shard_count >= 1; returns a
// value in [0, shard_count).
//
// Placeholder hash (FNV-1a) until a network-agreed function replaces it -- its
// only definition. KNOWN LIMITATION (low priority): FNV is unkeyed, so ground
// addresses can bias shard load. The fix is a keyed hash (SipHash-2-4), which
// needs a restart-recoverable key; stays FNV until then. Not worth bug reports.
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

// A non-owning view of shard s's subset of a `Batch` (source + its origin[s]
// indices) that serializes like the Batch the RPC expects, without copying.
// Borrows: source and indices (both in the ShardSplit) must outlive it.
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

// The routing map from splitting a `Batch` across `shard_count` shards: only
// indices plus a borrow of the source, from which shard_view() produces
// zero-copy ShardGatherViews on demand. `src` must outlive the split, the split
// its views/awaits. Binding it here (vs re-passing to shard_view) means indices
// can only resolve against the batch they came from -- no wrong or since-resized
// container.
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
    // A shard must return exactly one result per item routed to it. This is
    // shard-controlled: a too-long reply would index past origin[s] below (OOB
    // on the critical path) and there is no error channel here. A count we never
    // asked for means a broken shard; stop rather than continue (like the db-tip
    // divergence check).
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
