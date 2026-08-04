#pragma once

#include <cassert>
#include <cstddef>
#include <exception>
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

  // The default shard (keyless/global + content-addressed requests).
  Client &default_shard() { return clients_[kDefaultShard]; }

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
    std::vector<CallResult> in_flight;
    std::vector<size_t> shard_of;
    in_flight.reserve(clients_.size());
    shard_of.reserve(clients_.size());
    std::exception_ptr launch_error;
    try {
      for (size_t s = 0; s < clients_.size(); ++s) {
        if (views[s].empty()) {
          continue;
        }
        in_flight.push_back(call(clients_[s], views[s]));
        shard_of.push_back(s);
      }
    } catch (...) {
      launch_error = std::current_exception();
    }

    std::vector<ResultBatch> per_shard(clients_.size());
    co_await drain_awaits(in_flight, launch_error,
                          [&](size_t i, ResultBatch r) {
                            per_shard[shard_of[i]] = std::move(r);
                          });

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
    std::exception_ptr launch_error;
    try {
      for (size_t s = 0; s < clients_.size(); ++s) {
        in_flight.push_back(call(clients_[s]));
      }
    } catch (...) {
      launch_error = std::current_exception();
    }

    Status result = ok;
    co_await drain_awaits(in_flight, launch_error, [&](size_t, Status r) {
      if (result == ok && !(r == ok)) {
        result = r;
      }
    });
    co_return result;
  }

  // Like broadcast_status but keeps every shard's result (positionally, by
  // shard index) instead of reducing, so the caller can act on exactly which
  // shards succeeded -- e.g. roll back a partial create_snapshot. Concurrent
  // and drains on exception like the other helpers. call(client) ->
  // Async<Result>.
  template<typename Result, typename Call>
  x23::Async<std::vector<Result>> broadcast_collect(Call call)
  {
    using CallResult = decltype(call(clients_[0]));
    std::vector<CallResult> in_flight;
    in_flight.reserve(clients_.size());
    std::exception_ptr launch_error;
    try {
      for (size_t s = 0; s < clients_.size(); ++s) {
        in_flight.push_back(call(clients_[s]));
      }
    } catch (...) {
      launch_error = std::current_exception();
    }

    std::vector<Result> results(clients_.size());
    co_await drain_awaits(in_flight, launch_error, [&](size_t i, Result r) {
      results[i] = std::move(r);
    });
    co_return results;
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
    std::exception_ptr launch_error;
    try {
      for (size_t s = 0; s < clients_.size(); ++s) {
        if (views[s].empty()) {
          continue;
        }
        in_flight.push_back(call(clients_[s], views[s]));
      }
    } catch (...) {
      launch_error = std::current_exception();
    }

    Status result = ok;
    co_await drain_awaits(in_flight, launch_error, [&](size_t, Status r) {
      if (result == ok && !(r == ok)) {
        result = r;
      }
    });
    co_return result;
  }

private:
  // Await every launched Async in `in_flight`, calling visit(i, result) for
  // each while no exception is pending. `launch_error` carries any exception
  // thrown while launching (before all Asyncs existed). On any exception --
  // from a co_await here or from launch -- drain the remaining Asyncs before
  // rethrowing the first: destroying a still-running Async un-awaited is a UAF
  // (x23 ~Async), so a throw mid-gather must not abandon its siblings.
  template<typename InFlight, typename Visit>
  static x23::Async<void> drain_awaits(InFlight &in_flight,
                                       std::exception_ptr launch_error,
                                       Visit visit)
  {
    std::exception_ptr pending = std::move(launch_error);
    size_t i = 0;
    for (auto &async : in_flight) {
      try {
        auto result = co_await async;
        if (!pending) {
          visit(i, std::move(result));
        }
      } catch (...) {
        if (!pending) {
          pending = std::current_exception();
        }
      }
      ++i;
    }
    if (pending) {
      std::rethrow_exception(pending);
    }
  }

  std::vector<Client> clients_;
};

} // namespace hogd
