#pragma once

#include <cassert>
#include <cstddef>
#include <utility>
#include <vector>

#include "hogd/db_shard_routing/sharding.h"
#include "x23/x23/async.h"

namespace hogd {

// One client per shard, plus the async primitives for talking to a sharded
// database over a coroutine RPC interface. Consumers (the scheduler; any future
// async executor path) hold a ShardedClient<Client> instead of managing a
// per-shard client vector and routing by hand -- the routing and batch
// reassembly use the shared shard_router / sharded_batch core, so the sharding
// logic lives in one place.
//
// `Client` is whatever handle the caller holds per shard (a value client, or a
// std::shared_ptr<...> to one); the caller's `call`/`fn` lambdas dereference
// it, so both `client.method(...)` and `client->method(...)` styles work.
//
// NOTE: async-only. The executor's DatabaseProxy is synchronous and uses the
// pure split_by_shard / gather_positional core directly rather than this
// holder.
template<typename Client> class ShardedClient {
public:
  explicit ShardedClient(std::vector<Client> clients)
    : clients_(std::move(clients))
  {
    assert(!clients_.empty());
  }

  size_t shard_count() const { return clients_.size(); }

  // The primary shard, for global/keyless requests (db tip, snapshots) and
  // content-addressed lookups.
  Client &primary() { return clients_[kPrimaryShard]; }

  // Scatter a keyed batch across shards and gather the per-item results back
  // into the ORIGINAL input order.
  //   route(item, shard_count) -> owning shard index in [0, shard_count)
  //   call(client, sub_batch)  -> x23::Async<ResultBatch>, one result per item
  //                               in sub_batch order
  // Shards with no items in this batch are skipped (independent reads need no
  // cross-shard alignment). Returns a ResultBatch of length items.size().
  //
  // The touched shards are issued concurrently: an Async begins executing on
  // creation (initial_suspend is suspend_never), so launching every shard's
  // call before awaiting any keeps all the round-trips in flight at once --
  // wall-clock is one round-trip, not N. `split` outlives every await, so a
  // sub-batch passed by reference into `call` stays valid throughout.
  template<typename ResultBatch, typename Batch, typename Route, typename Call>
  x23::Async<ResultBatch> scatter_gather(Batch const &items, Route route,
                                         Call call)
  {
    auto split = split_by_shard(items, clients_.size(), route);
    // Zero-copy per-shard views over `items`; `split` and `items` outlive every
    // await below, so the views (and the source they borrow) stay valid.
    std::vector<ShardGatherView<Batch>> views;
    views.reserve(clients_.size());
    for (size_t s = 0; s < clients_.size(); ++s) {
      views.push_back(split.shard_view(items, s));
    }
    using CallResult = decltype(call(clients_[0], views[0]));
    std::vector<std::pair<size_t, CallResult>> in_flight;
    in_flight.reserve(clients_.size());
    for (size_t s = 0; s < clients_.size(); ++s) {
      if (views[s].empty()) {
        continue;
      }
      in_flight.emplace_back(s, call(clients_[s], views[s]));
    }

    std::vector<ResultBatch> per_shard(clients_.size());
    for (auto &[s, async] : in_flight) {
      per_shard[s] = co_await async;
    }

    std::vector<typename ResultBatch::value_type> ordered =
        gather_positional(split, per_shard);
    ResultBatch out;
    out.reserve(ordered.size());
    for (auto &r : ordered) {
      out.push_back(std::move(r));
    }
    co_return out;
  }

  // Fan the SAME call out to EVERY shard (no per-shard batching) and reduce the
  // per-shard statuses to one: the first status not equal to `ok`, or `ok` if
  // every shard succeeds. Every shard is visited even after one fails -- these
  // are the homogeneous block-lifecycle fan-outs (open/abandon a block) that
  // must reach all shards so a later abandon can clean them all up uniformly.
  // Contrast scatter_write, which is keyed (per-shard sub-batches).
  //   call(client) -> x23::Async<Status>
  template<typename Status, typename Call>
  x23::Async<Status> broadcast_status(Call call, Status ok)
  {
    Status result = ok;
    for (size_t s = 0; s < clients_.size(); ++s) {
      Status r = co_await call(clients_[s]);
      if (result == ok && !(r == ok)) {
        result = r;
      }
    }
    co_return result;
  }

  // Scatter a keyed batch across shards and reduce the per-shard statuses to
  // one. Returns the first (lowest-shard) status not equal to `ok`, or `ok` if
  // every touched shard succeeds. Shards with no items are skipped.
  //   route(item, shard_count) -> owning shard index
  //   call(client, sub_batch)  -> x23::Async<Status>
  //
  // The touched shards are issued concurrently (see scatter_gather): every
  // shard's call is launched before any is awaited, so N shards cost one
  // round-trip, not N. All touched shards are visited even after one fails --
  // the writes are already in flight, and on a non-ok result the caller aborts,
  // which fans out to every shard anyway.
  template<typename Status, typename Batch, typename Route, typename Call>
  x23::Async<Status> scatter_write(Batch const &items, Route route, Call call,
                                   Status ok)
  {
    auto split = split_by_shard(items, clients_.size(), route);
    // Zero-copy per-shard views over `items`; `split` and `items` outlive every
    // await below, so the views (and the source they borrow) stay valid.
    std::vector<ShardGatherView<Batch>> views;
    views.reserve(clients_.size());
    for (size_t s = 0; s < clients_.size(); ++s) {
      views.push_back(split.shard_view(items, s));
    }
    using CallResult = decltype(call(clients_[0], views[0]));
    std::vector<CallResult> in_flight;
    in_flight.reserve(clients_.size());
    for (size_t s = 0; s < clients_.size(); ++s) {
      if (views[s].empty()) {
        continue;
      }
      in_flight.push_back(call(clients_[s], views[s]));
    }

    Status result = ok;
    for (auto &async : in_flight) {
      Status r = co_await async;
      if (result == ok && !(r == ok)) {
        result = r;
      }
    }
    co_return result;
  }

private:
  std::vector<Client> clients_;
};

} // namespace hogd
