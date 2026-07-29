#pragma once

#include <cassert>
#include <cstddef>
#include <utility>
#include <vector>

#include "hogd/db_shard_routing/sharding.h"
#include "x23/x23/async.h"

namespace hogd {

// One client per shard plus the async fan-out primitives, routing via
// sharding.h. `Client` is any per-shard handle; the caller's lambdas
// dereference it. Async-only (the executor's DatabaseProxy uses the sharding.h
// core).
template<typename Client> class ShardedClient {
public:
  explicit ShardedClient(std::vector<Client> clients)
    : clients_(std::move(clients))
  {
    assert(!clients_.empty());
  }

  size_t shard_count() const { return clients_.size(); }

  // The primary shard (global/keyless + content-addressed requests).
  Client &primary() { return clients_[kPrimaryShard]; }

  // Shard s's client, for fan-outs that don't reduce through the helpers
  // (genesis load, close/commit snapshot). Precondition: s < shard_count().
  Client &shard(size_t s)
  {
    assert(s < clients_.size());
    return clients_[s];
  }

  // Scatter a keyed batch (empty shards skipped) and gather results into input
  // order. call(client, sub_batch) -> Async<ResultBatch>. Every shard's call is
  // launched before any is awaited, so N shards cost one round-trip, not N.
  template<typename ResultBatch, typename Batch, typename Route, typename Call>
  x23::Async<ResultBatch> scatter_gather(Batch const &items, Route route,
                                         Call call)
  {
    auto split = split_by_shard(items, clients_.size(), route);
    // Views outlive every await below.
    std::vector<ShardGatherView<Batch>> views;
    views.reserve(clients_.size());
    for (size_t s = 0; s < clients_.size(); ++s) {
      views.push_back(split.shard_view(s));
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

  // Fan the same call out to every shard (visiting all even after a failure)
  // and reduce to the first non-`ok` status. For block-lifecycle fan-outs.
  // Concurrent like scatter_gather. call(client) -> Async<Status>.
  template<typename Status, typename Call>
  x23::Async<Status> broadcast_status(Call call, Status ok)
  {
    using CallResult = decltype(call(clients_[0]));
    std::vector<CallResult> in_flight;
    in_flight.reserve(clients_.size());
    for (size_t s = 0; s < clients_.size(); ++s) {
      in_flight.push_back(call(clients_[s]));
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

  // Keyed counterpart to broadcast_status: scatter to owning shards and reduce
  // to the first non-`ok` status. call(client, sub_batch) -> Async<Status>.
  template<typename Status, typename Batch, typename Route, typename Call>
  x23::Async<Status> scatter_write(Batch const &items, Route route, Call call,
                                   Status ok)
  {
    auto split = split_by_shard(items, clients_.size(), route);
    // Views outlive every await below.
    std::vector<ShardGatherView<Batch>> views;
    views.reserve(clients_.size());
    for (size_t s = 0; s < clients_.size(); ++s) {
      views.push_back(split.shard_view(s));
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
