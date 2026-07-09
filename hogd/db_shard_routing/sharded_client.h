#pragma once

#include <cassert>
#include <cstddef>
#include <utility>
#include <vector>

#include "hogd/db_shard_routing/shard_router.h"
#include "hogd/db_shard_routing/sharded_batch.h"
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
// std::shared_ptr<...> to one); the caller's `call`/`fn` lambdas dereference it,
// so both `client.method(...)` and `client->method(...)` styles work.
//
// NOTE: async-only. The executor's DatabaseProxy is synchronous and uses the
// pure split_by_shard / gather_positional core directly rather than this holder.
template <typename Client>
class ShardedClient {
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
  template <typename ResultBatch, typename Batch, typename Route, typename Call>
  x23::Async<ResultBatch> scatter_gather(Batch const &items, Route route,
                                         Call call)
  {
    auto split = split_by_shard(items, clients_.size(), route);
    std::vector<ResultBatch> per_shard(clients_.size());
    for (size_t s = 0; s < clients_.size(); ++s) {
      if (split.batches[s].empty()) {
        continue;
      }
      per_shard[s] = co_await call(clients_[s], split.batches[s]);
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

  // Scatter a keyed batch across shards and reduce the per-shard statuses to
  // one. Returns the first status not equal to `ok` (stopping there), or `ok`
  // if every touched shard succeeds. Shards with no items are skipped.
  //   route(item, shard_count) -> owning shard index
  //   call(client, sub_batch)  -> x23::Async<Status>
  template <typename Status, typename Batch, typename Route, typename Call>
  x23::Async<Status> scatter_write(Batch const &items, Route route, Call call,
                                   Status ok)
  {
    auto split = split_by_shard(items, clients_.size(), route);
    for (size_t s = 0; s < clients_.size(); ++s) {
      if (split.batches[s].empty()) {
        continue;
      }
      Status r = co_await call(clients_[s], split.batches[s]);
      if (!(r == ok)) {
        co_return r;
      }
    }
    co_return ok;
  }

private:
  std::vector<Client> clients_;
};

} // namespace hogd
