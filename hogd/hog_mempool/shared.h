#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <vector>

#include "auth_rpc.hh"
#include "coord_sched.hh"
#include "db_sched.hh"
#include "event_rpc.hh"
#include "exec_sched.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/time.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/crypto/crypto.h"
#include "hogd/db_shard_routing/sharded_client.h"
#include "hogd/metrics/metrics.h"
#include "hogd/xdr/proposal_hash.h"
#include "loadgen.hh"
#include "parallel.hh"
#include "proposal.hh"
#include "sched_coord.hh"
#include "sched_db.hh"
#include "transaction.hh"
#include "x23/async.h"
#include "x23/rpc.h"
#include "x23/serialize.h"
#include "x23/sockio.h"
#include "x23/vec.h"

namespace hogd {

using auth_sched_client_t =
    AuthMempool1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

using coord_sched_client_t =
    CoordScheduler1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

using db_sched_client_t =
    DbSched1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

// The scheduler's sharding-aware view of the database: one db-sched client per
// shard, with per-account routing / scatter-gather for the hog-age RPCs (see
// scheduler.cc). With a single shard this behaves like the lone shard client.
using sharded_db_sched_client_t =
    ShardedClient<std::shared_ptr<db_sched_client_t>>;

using event_sched_client_t =
    EventSched1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

using exec_sched_client_t =
    ExecutorSched1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

using loadgen_client_t =
    LoadgenInterfaceV1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

using sched_coord_client_t =
    SchedulerCoord1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

using sched_db_client_t =
    SchedDb1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

template<typename T>
inline auto
create_client(const sockaddr_storage &ss, bool retry = false,
              size_t max_message_size = x23::default_max_message_size)
{
  return make_rpc_client<T>(ss, retry, max_message_size);
}

template<typename T>
inline auto
create_client(x23::HostPort &host_port, bool retry = false,
              size_t max_message_size = x23::default_max_message_size)
{
  auto ss_or_error = host_port.to_addr(/*dns=*/true);
  CHECK(ss_or_error.has_value());
  return create_client<T>(*ss_or_error, retry, max_message_size);
}

template<typename T>
inline auto
create_client(const std::string &host, uint16_t port, bool retry = false,
              size_t max_message_size = x23::default_max_message_size)
{
  x23::HostPort host_port{host.c_str(), port};
  return create_client<T>(host_port, retry, max_message_size);
}

class MempoolMetrics {
public:
  static MempoolMetrics &instance()
  {
    static MempoolMetrics metrics;

    // Initialize the metrics library on the first call to this function.
    Metrics &registry = Metrics::instance();
    if (!registry.done_with_init(Metrics::ComponentName::kMempool)) {
      static std::mutex init_mutex;
      std::lock_guard<std::mutex> lock(init_mutex);
      if (registry.done_with_init(Metrics::ComponentName::kMempool)) {
        return metrics;
      }
      metrics.init();
    }
    return metrics;
  }

  Metrics::Gauge current_block_height;
  Metrics::Gauge scheduler_pending_tx_count;
  Metrics::Gauge scheduler_tx_cache_size;
  Metrics::Gauge validation_in_progress_count;
  Metrics::Gauge last_submit_unvetted_duration_ms;
  Metrics::Gauge last_submit_vetted_duration_ms;
  Metrics::Gauge last_nomination_duration_ms;
  Metrics::Gauge last_validation_duration_ms;
  Metrics::Gauge last_commit_duration_ms;
  Metrics::Gauge last_commit_interval_ms;
  Metrics::Gauge last_scheduler_remove_duration_ms;

  Metrics::Counter submitted_unvetted_tx_count;
  Metrics::Counter submitted_vetted_tx_count;
  Metrics::Counter accepted_unvetted_tx_count;
  Metrics::Counter accepted_vetted_tx_count;
  Metrics::Counter discarded_capacity_tx_count;
  Metrics::Counter rejected_capacity_rpc_count;
  Metrics::Counter rejected_malformed_unvetted_rpc_count;
  Metrics::Counter rejected_malformed_vetted_rpc_count;
  Metrics::Counter auth_failed_tx_count;
  Metrics::Counter nomination_attempt_count;
  Metrics::Counter nomination_success_count;
  Metrics::Counter nomination_error_count;
  Metrics::Counter nomination_scheduled_tx_count;
  Metrics::Counter nomination_nominated_tx_count;
  Metrics::Counter nomination_failed_tx_count;
  Metrics::Counter validation_attempt_count;
  Metrics::Counter validation_ok_count;
  Metrics::Counter validation_invalid_count;
  Metrics::Counter validation_too_late_count;
  Metrics::Counter validation_missing_txs_count;
  Metrics::Counter validation_unknown_parent_count;
  Metrics::Counter validation_db_failed_count;
  Metrics::Counter validation_executor_failed_count;
  Metrics::Counter validation_malformed_count;
  Metrics::Counter validation_tx_count;
  Metrics::Counter commit_attempt_count;
  Metrics::Counter commit_success_count;
  Metrics::Counter commit_block_not_found_count;
  Metrics::Counter commit_reenqueued_tx_count;
  Metrics::Counter commit_removed_tx_count;

private:
  void init();
};

inline ssize_t
mempool_duration_ms(double seconds)
{
  return static_cast<ssize_t>(seconds * 1000.0);
}

inline void
MempoolMetrics::init()
{
  Metrics &metrics = Metrics::instance();
  constexpr Metrics::ComponentName kComponent =
      Metrics::ComponentName::kMempool;

  current_block_height =
      metrics.gauge(Metrics::ComponentName::kMempool, "current_block_height",
                    "The height of the last block committed by the mempool.");
  scheduler_pending_tx_count = metrics.gauge(
      kComponent, "scheduler_pending_tx_count",
      "The number of transactions currently pending in the scheduler.");
  scheduler_tx_cache_size =
      metrics.gauge(kComponent, "scheduler_tx_cache_size",
                    "The number of transactions cached by transaction hash.");
  validation_in_progress_count =
      metrics.gauge(kComponent, "validation_in_progress_count",
                    "The number of block validations currently in progress.");
  last_submit_unvetted_duration_ms = metrics.gauge(
      kComponent, "last_submit_unvetted_duration_ms",
      "The duration of the last unvetted transaction submission RPC in "
      "milliseconds.");
  last_submit_vetted_duration_ms = metrics.gauge(
      kComponent, "last_submit_vetted_duration_ms",
      "The duration of the last vetted transaction submission RPC in "
      "milliseconds.");
  last_nomination_duration_ms =
      metrics.gauge(kComponent, "last_nomination_duration_ms",
                    "The duration of the last nomination RPC in milliseconds.");
  last_validation_duration_ms =
      metrics.gauge(kComponent, "last_validation_duration_ms",
                    "The duration of the last validation RPC in milliseconds.");
  last_commit_duration_ms =
      metrics.gauge(kComponent, "last_commit_duration_ms",
                    "The duration of the last commit RPC in milliseconds.");
  last_commit_interval_ms = metrics.gauge(
      kComponent, "last_commit_interval_ms",
      "The interval from the end of the previous commit to the start of the "
      "last commit in milliseconds.");
  last_scheduler_remove_duration_ms = metrics.gauge(
      kComponent, "last_scheduler_remove_duration_ms",
      "The duration of the last scheduler removal pass in milliseconds.");

  submitted_unvetted_tx_count = metrics.per_thread_counter(
      kComponent, "submitted_unvetted_tx_count",
      "Transactions submitted through the unvetted submission RPC.");
  submitted_vetted_tx_count = metrics.per_thread_counter(
      kComponent, "submitted_vetted_tx_count",
      "Transactions submitted through the vetted submission RPC.");
  accepted_unvetted_tx_count = metrics.per_thread_counter(
      kComponent, "accepted_unvetted_tx_count",
      "Unvetted submitted transactions accepted into the scheduler.");
  accepted_vetted_tx_count = metrics.per_thread_counter(
      kComponent, "accepted_vetted_tx_count",
      "Vetted submitted transactions accepted into the scheduler.");
  discarded_capacity_tx_count = metrics.per_thread_counter(
      kComponent, "discarded_capacity_tx_count",
      "Unvetted submitted transactions discarded because the mempool was at "
      "capacity.");
  rejected_capacity_rpc_count = metrics.per_thread_counter(
      kComponent, "rejected_capacity_rpc_count",
      "Unvetted submission RPCs rejected because the mempool was at capacity.");
  rejected_malformed_unvetted_rpc_count = metrics.per_thread_counter(
      kComponent, "rejected_malformed_unvetted_rpc_count",
      "Unvetted submission RPCs rejected due to malformed transaction "
      "batches.");
  rejected_malformed_vetted_rpc_count = metrics.per_thread_counter(
      kComponent, "rejected_malformed_vetted_rpc_count",
      "Vetted submission RPCs rejected due to malformed transaction "
      "batches.");
  auth_failed_tx_count = metrics.per_thread_counter(
      kComponent, "auth_failed_tx_count",
      "Submitted transactions rejected by authentication.");
  nomination_attempt_count =
      metrics.per_thread_counter(kComponent, "nomination_attempt_count",
                                 "Block nomination attempts.");
  nomination_success_count =
      metrics.per_thread_counter(kComponent, "nomination_success_count",
                                 "Successful block nominations.");
  nomination_error_count =
      metrics.per_thread_counter(kComponent, "nomination_error_count",
                                 "Failed block nominations.");
  nomination_scheduled_tx_count = metrics.per_thread_counter(
      kComponent, "nomination_scheduled_tx_count",
      "Transactions scheduled during nomination attempts.");
  nomination_nominated_tx_count = metrics.per_thread_counter(
      kComponent, "nomination_nominated_tx_count",
      "Transactions included in successful nominations.");
  nomination_failed_tx_count = metrics.per_thread_counter(
      kComponent, "nomination_failed_tx_count",
      "Transactions scheduled for nomination that failed proposal execution.");
  validation_attempt_count =
      metrics.per_thread_counter(kComponent, "validation_attempt_count",
                                 "Block validation attempts.");
  validation_ok_count =
      metrics.per_thread_counter(kComponent, "validation_ok_count",
                                 "Successful block validations.");
  validation_invalid_count =
      metrics.per_thread_counter(kComponent, "validation_invalid_count",
                                 "Invalid block validations.");
  validation_too_late_count = metrics.per_thread_counter(
      kComponent, "validation_too_late_count",
      "Validation requests for blocks older than the current mempool height.");
  validation_missing_txs_count = metrics.per_thread_counter(
      kComponent, "validation_missing_txs_count",
      "Validation requests that could not complete due to missing "
      "transactions.");
  validation_unknown_parent_count = metrics.per_thread_counter(
      kComponent, "validation_unknown_parent_count",
      "Validation requests whose parent block was unknown to the database.");
  validation_db_failed_count = metrics.per_thread_counter(
      kComponent, "validation_db_failed_count",
      "Validation requests that failed due to database errors.");
  validation_executor_failed_count = metrics.per_thread_counter(
      kComponent, "validation_executor_failed_count",
      "Validation requests that failed due to executor errors.");
  validation_malformed_count = metrics.per_thread_counter(
      kComponent, "validation_malformed_count",
      "Validation requests rejected due to malformed nominations.");
  validation_tx_count = metrics.per_thread_counter(
      kComponent, "validation_tx_count",
      "Transactions included in validation requests.");
  commit_attempt_count =
      metrics.per_thread_counter(kComponent, "commit_attempt_count",
                                 "Block commit attempts.");
  commit_success_count =
      metrics.per_thread_counter(kComponent, "commit_success_count",
                                 "Successful block commits.");
  commit_block_not_found_count = metrics.per_thread_counter(
      kComponent, "commit_block_not_found_count",
      "Commit requests for blocks that were not nominated or validated by "
      "this mempool.");
  commit_reenqueued_tx_count = metrics.per_thread_counter(
      kComponent, "commit_reenqueued_tx_count",
      "Transactions returned to the scheduler after another block committed.");
  commit_removed_tx_count = metrics.per_thread_counter(
      kComponent, "commit_removed_tx_count",
      "Transactions removed from the scheduler after block commits.");

  metrics.mark_done_with_init(kComponent);
}

} // namespace hogd
