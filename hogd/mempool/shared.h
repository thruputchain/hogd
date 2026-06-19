#pragma once

#include <cstdint>
#include <string>
#include <sys/socket.h>
#include <vector>

#include "auth_rpc.hh"
#include "block.hh"
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
#include "hogd/metrics/metrics.h"
#include "loadgen.hh"
#include "parallel.hh"
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

inline SHA256Hash
generate_block_hash(const BlockHeader &header)
{
  return sha256(header);
}

inline Metrics::Gauge mempool_current_block_height_metric;
inline Metrics::Gauge mempool_scheduler_pending_tx_count_metric;
inline Metrics::Gauge mempool_scheduler_tx_cache_size_metric;
inline Metrics::Gauge mempool_validation_in_progress_count_metric;
inline Metrics::Gauge mempool_last_submit_unvetted_duration_ms_metric;
inline Metrics::Gauge mempool_last_submit_vetted_duration_ms_metric;
inline Metrics::Gauge mempool_last_nomination_duration_ms_metric;
inline Metrics::Gauge mempool_last_validation_duration_ms_metric;
inline Metrics::Gauge mempool_last_commit_duration_ms_metric;
inline Metrics::Gauge mempool_last_commit_interval_ms_metric;
inline Metrics::Gauge mempool_last_scheduler_remove_duration_ms_metric;

inline Metrics::Counter mempool_submitted_unvetted_tx_count_metric;
inline Metrics::Counter mempool_submitted_vetted_tx_count_metric;
inline Metrics::Counter mempool_accepted_unvetted_tx_count_metric;
inline Metrics::Counter mempool_accepted_vetted_tx_count_metric;
inline Metrics::Counter mempool_discarded_capacity_tx_count_metric;
inline Metrics::Counter mempool_rejected_capacity_rpc_count_metric;
inline Metrics::Counter mempool_rejected_malformed_unvetted_rpc_count_metric;
inline Metrics::Counter mempool_rejected_malformed_vetted_rpc_count_metric;
inline Metrics::Counter mempool_auth_failed_tx_count_metric;
inline Metrics::Counter mempool_nomination_attempt_count_metric;
inline Metrics::Counter mempool_nomination_success_count_metric;
inline Metrics::Counter mempool_nomination_error_count_metric;
inline Metrics::Counter mempool_nomination_scheduled_tx_count_metric;
inline Metrics::Counter mempool_nomination_nominated_tx_count_metric;
inline Metrics::Counter mempool_nomination_failed_tx_count_metric;
inline Metrics::Counter mempool_validation_attempt_count_metric;
inline Metrics::Counter mempool_validation_ok_count_metric;
inline Metrics::Counter mempool_validation_invalid_count_metric;
inline Metrics::Counter mempool_validation_too_late_count_metric;
inline Metrics::Counter mempool_validation_missing_txs_count_metric;
inline Metrics::Counter mempool_validation_unknown_parent_count_metric;
inline Metrics::Counter mempool_validation_db_failed_count_metric;
inline Metrics::Counter mempool_validation_executor_failed_count_metric;
inline Metrics::Counter mempool_validation_malformed_count_metric;
inline Metrics::Counter mempool_validation_tx_count_metric;
inline Metrics::Counter mempool_commit_attempt_count_metric;
inline Metrics::Counter mempool_commit_success_count_metric;
inline Metrics::Counter mempool_commit_block_not_found_count_metric;
inline Metrics::Counter mempool_commit_reenqueued_tx_count_metric;
inline Metrics::Counter mempool_commit_removed_tx_count_metric;

inline ssize_t
mempool_duration_ms(double seconds)
{
  return static_cast<ssize_t>(seconds * 1000.0);
}

inline void
init_metrics()
{
  Metrics &metrics = Metrics::instance();
  constexpr Metrics::ComponentName component = Metrics::ComponentName::kMempool;

  mempool_current_block_height_metric =
      metrics.gauge(Metrics::ComponentName::kMempool, "current_block_height",
                    "The height of the last block committed by the mempool.");
  mempool_scheduler_pending_tx_count_metric = metrics.gauge(
      component, "scheduler_pending_tx_count",
      "The number of transactions currently pending in the scheduler.");
  mempool_scheduler_tx_cache_size_metric =
      metrics.gauge(component, "scheduler_tx_cache_size",
                    "The number of transactions cached by transaction hash.");
  mempool_validation_in_progress_count_metric =
      metrics.gauge(component, "validation_in_progress_count",
                    "The number of block validations currently in progress.");
  mempool_last_submit_unvetted_duration_ms_metric = metrics.gauge(
      component, "last_submit_unvetted_duration_ms",
      "The duration of the last unvetted transaction submission RPC in "
      "milliseconds.");
  mempool_last_submit_vetted_duration_ms_metric = metrics.gauge(
      component, "last_submit_vetted_duration_ms",
      "The duration of the last vetted transaction submission RPC in "
      "milliseconds.");
  mempool_last_nomination_duration_ms_metric =
      metrics.gauge(component, "last_nomination_duration_ms",
                    "The duration of the last nomination RPC in milliseconds.");
  mempool_last_validation_duration_ms_metric =
      metrics.gauge(component, "last_validation_duration_ms",
                    "The duration of the last validation RPC in milliseconds.");
  mempool_last_commit_duration_ms_metric =
      metrics.gauge(component, "last_commit_duration_ms",
                    "The duration of the last commit RPC in milliseconds.");
  mempool_last_commit_interval_ms_metric = metrics.gauge(
      component, "last_commit_interval_ms",
      "The interval from the end of the previous commit to the start of the "
      "last commit in milliseconds.");
  mempool_last_scheduler_remove_duration_ms_metric = metrics.gauge(
      component, "last_scheduler_remove_duration_ms",
      "The duration of the last scheduler removal pass in milliseconds.");

  mempool_submitted_unvetted_tx_count_metric = metrics.per_thread_counter(
      component, "submitted_unvetted_tx_count",
      "Transactions submitted through the unvetted submission RPC.");
  mempool_submitted_vetted_tx_count_metric = metrics.per_thread_counter(
      component, "submitted_vetted_tx_count",
      "Transactions submitted through the vetted submission RPC.");
  mempool_accepted_unvetted_tx_count_metric = metrics.per_thread_counter(
      component, "accepted_unvetted_tx_count",
      "Unvetted submitted transactions accepted into the scheduler.");
  mempool_accepted_vetted_tx_count_metric = metrics.per_thread_counter(
      component, "accepted_vetted_tx_count",
      "Vetted submitted transactions accepted into the scheduler.");
  mempool_discarded_capacity_tx_count_metric = metrics.per_thread_counter(
      component, "discarded_capacity_tx_count",
      "Unvetted submitted transactions discarded because the mempool was at "
      "capacity.");
  mempool_rejected_capacity_rpc_count_metric = metrics.per_thread_counter(
      component, "rejected_capacity_rpc_count",
      "Unvetted submission RPCs rejected because the mempool was at capacity.");
  mempool_rejected_malformed_unvetted_rpc_count_metric =
      metrics.per_thread_counter(
          component, "rejected_malformed_unvetted_rpc_count",
          "Unvetted submission RPCs rejected due to malformed transaction "
          "batches.");
  mempool_rejected_malformed_vetted_rpc_count_metric =
      metrics.per_thread_counter(
          component, "rejected_malformed_vetted_rpc_count",
          "Vetted submission RPCs rejected due to malformed transaction "
          "batches.");
  mempool_auth_failed_tx_count_metric = metrics.per_thread_counter(
      component, "auth_failed_tx_count",
      "Submitted transactions rejected by authentication.");
  mempool_nomination_attempt_count_metric =
      metrics.per_thread_counter(component, "nomination_attempt_count",
                                 "Block nomination attempts.");
  mempool_nomination_success_count_metric =
      metrics.per_thread_counter(component, "nomination_success_count",
                                 "Successful block nominations.");
  mempool_nomination_error_count_metric =
      metrics.per_thread_counter(component, "nomination_error_count",
                                 "Failed block nominations.");
  mempool_nomination_scheduled_tx_count_metric = metrics.per_thread_counter(
      component, "nomination_scheduled_tx_count",
      "Transactions scheduled during nomination attempts.");
  mempool_nomination_nominated_tx_count_metric = metrics.per_thread_counter(
      component, "nomination_nominated_tx_count",
      "Transactions included in successful nominations.");
  mempool_nomination_failed_tx_count_metric = metrics.per_thread_counter(
      component, "nomination_failed_tx_count",
      "Transactions scheduled for nomination that failed proposal execution.");
  mempool_validation_attempt_count_metric =
      metrics.per_thread_counter(component, "validation_attempt_count",
                                 "Block validation attempts.");
  mempool_validation_ok_count_metric =
      metrics.per_thread_counter(component, "validation_ok_count",
                                 "Successful block validations.");
  mempool_validation_invalid_count_metric =
      metrics.per_thread_counter(component, "validation_invalid_count",
                                 "Invalid block validations.");
  mempool_validation_too_late_count_metric = metrics.per_thread_counter(
      component, "validation_too_late_count",
      "Validation requests for blocks older than the current mempool height.");
  mempool_validation_missing_txs_count_metric = metrics.per_thread_counter(
      component, "validation_missing_txs_count",
      "Validation requests that could not complete due to missing "
      "transactions.");
  mempool_validation_unknown_parent_count_metric = metrics.per_thread_counter(
      component, "validation_unknown_parent_count",
      "Validation requests whose parent block was unknown to the database.");
  mempool_validation_db_failed_count_metric = metrics.per_thread_counter(
      component, "validation_db_failed_count",
      "Validation requests that failed due to database errors.");
  mempool_validation_executor_failed_count_metric = metrics.per_thread_counter(
      component, "validation_executor_failed_count",
      "Validation requests that failed due to executor errors.");
  mempool_validation_malformed_count_metric = metrics.per_thread_counter(
      component, "validation_malformed_count",
      "Validation requests rejected due to malformed nominations.");
  mempool_validation_tx_count_metric = metrics.per_thread_counter(
      component, "validation_tx_count",
      "Transactions included in validation requests.");
  mempool_commit_attempt_count_metric =
      metrics.per_thread_counter(component, "commit_attempt_count",
                                 "Block commit attempts.");
  mempool_commit_success_count_metric =
      metrics.per_thread_counter(component, "commit_success_count",
                                 "Successful block commits.");
  mempool_commit_block_not_found_count_metric = metrics.per_thread_counter(
      component, "commit_block_not_found_count",
      "Commit requests for blocks that were not nominated or validated by "
      "this mempool.");
  mempool_commit_reenqueued_tx_count_metric = metrics.per_thread_counter(
      component, "commit_reenqueued_tx_count",
      "Transactions returned to the scheduler after another block committed.");
  mempool_commit_removed_tx_count_metric = metrics.per_thread_counter(
      component, "commit_removed_tx_count",
      "Transactions removed from the scheduler after block commits.");

  metrics.mark_done_with_init(component);
}

} // namespace hogd
