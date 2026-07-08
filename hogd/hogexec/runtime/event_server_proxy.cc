#include "hogd/hogexec/runtime/event_server_proxy.h"

#include <atomic>
#include <memory>
#include <utility>

#include "db_conflict.hh"
#include "event.hh"
#include "event_rpc.hh"
#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "x23/async.h"
#include "x23/cpumap.h"
#include "x23/sockio.h"
#include "x23/vec.h"

namespace hogd {

namespace {

auto
connect_to_event_server(x23::HostPort const &host_port, x23::CpuMap const &map)
{
  info("Connecting to event server on host port {}", host_port.show());
  return make_rpc_multiclient<EventExecutor1>(host_port, true, map);
}

} // namespace

EventServerProxy::EventServerProxy(x23::HostPort const &host_port,
                                   x23::CpuMap const &map)
  : client_(connect_to_event_server(host_port, map))
{}

PerTransactionEventServerProxy::PerTransactionEventServerProxy(
    EventServerProxy &event_server_proxy, BlockID block_id, TransactionID tx_id)
  : event_server_proxy_(event_server_proxy), block_id_(block_id), tx_id_(tx_id)
{}

x23::Async<SystemStatus<void>>
EventServerProxy::open_block(BlockID block_id)
{
  auto r = co_await client_.eventexecutor_open_block(block_id);
  if (r != RPCError::kRPCNone) {
    error("open_block failed: {}", r);
  }
  switch (r) {
  case RPCError::kRPCNone:
    co_return SystemStatus<void>{};
  case RPCError::kRPCBadBlockID:
    co_return SystemStatus<void>{SystemError{SystemErrorCode::Abandon}};
  default:
    error("Unrecoverable: Received unhandled error type from event server: {}",
          r);
    co_return SystemStatus<void>{SystemError{SystemErrorCode::Unrecoverable}};
  }
}

x23::Async<SystemStatus<void>>
EventServerProxy::close_block(BlockID block_id)
{
  auto r = co_await client_.eventexecutor_close_block(block_id);
  if (r != RPCError::kRPCNone) {
    error("close_block failed: {}", r);
  }
  switch (r) {
  case RPCError::kRPCNone:
    co_return SystemStatus<void>{};
  case RPCError::kRPCBadBlockID:
    co_return SystemStatus<void>{SystemError{SystemErrorCode::Abandon}};
  default:
    error("Unrecoverable: Received unhandled error type from event server: {}",
          r);
    co_return SystemStatus<void>{SystemError{SystemErrorCode::Unrecoverable}};
  }
}

x23::Async<SystemStatus<void>>
EventServerProxy::emit_event(BlockID block_id, TransactionID tx_id,
                             TxEventLog const &tx_event_log,
                             ProposedTransaction const &proposed_tx)
{
  RPCError r = RPCError::kRPCNone;
  try {
    r = co_await client_.eventexecutor_emit_event(block_id, tx_id, tx_event_log,
                                                  proposed_tx);
  } catch (x23::XdrException const &e) {
    // The RPC client can throw (an over-cap payload throws XdrBadInput; a
    // failed call throws XdrCallFailed) instead of returning a status; on this
    // detached event coroutine an uncaught throw would terminate the executor
    // (#2857). Catch the XDR base so no RPC-layer throw escapes; anything else
    // (e.g. bad_alloc) is a genuine bug and still terminates.
    error("Unrecoverable: event emission threw before an RPC status: {}",
          e.what());
    co_return SystemStatus<void>{SystemError{SystemErrorCode::Unrecoverable}};
  }
  if (r != RPCError::kRPCNone) {
    error("emit_event failed: {}", r);
  }
  switch (r) {
  case RPCError::kRPCNone:
    co_return SystemStatus<void>{};
  case RPCError::kRPCBadBlockID:
    co_return SystemStatus<void>{SystemError{SystemErrorCode::Abandon}};
  default:
    error("Unrecoverable: Received unhandled error type from event server: {}",
          r);
    co_return SystemStatus<void>{SystemError{SystemErrorCode::Unrecoverable}};
  }
}

x23::Async<SystemStatus<void>>
PerTransactionEventServerProxy::emit_events(
    TxEventLog const &tx_event_log, ProposedTransaction const &proposed_tx)
{
  if (emitted_event_) {
    co_return SystemStatus<void>{SystemError{SystemErrorCode::Unrecoverable}};
  }
  emitted_event_ = true;
  co_return co_await event_server_proxy_.emit_event(block_id_, tx_id_,
                                                    tx_event_log, proposed_tx);
}

void
EventServerProxy::report_tx_failure(SignedTransactionHash tx_hash,
                                    TxFailureDiagnostic diag)
{
  // Reserve a slot with a single RMW; release it and drop the report if we
  // were already at the cap. Dropping (rather than queueing unboundedly) keeps
  // the fire-and-forget path memory-bounded when the event server is slow.
  if (pending_failure_reports_->fetch_add(1, std::memory_order_acq_rel) >=
      kMaxPendingFailureReports) {
    pending_failure_reports_->fetch_sub(1, std::memory_order_release);
    return;
  }
  // The coroutine below takes a refcounted copy of client_ and a shared_ptr
  // to the pending-count atomic (not `this`/`&self`), so it never touches
  // this EventServerProxy and can safely outlive it.
  [](client_t client, std::shared_ptr<std::atomic<ssize_t>> pending,
     SignedTransactionHash hash, TxFailureDiagnostic d) -> x23::Detached {
    try {
      RPCError r = co_await client.eventexecutor_report_tx_failure(hash, d);
      if (r != RPCError::kRPCNone) {
        warn("tx-failure report dropped by event server: {}", r);
      }
    } catch (x23::XdrException const &e) {
      // Best-effort: the RPC client can throw (XdrBadInput / XdrCallFailed)
      // instead of returning a status; swallow the RPC-layer throw so it never
      // escapes this detached coroutine, and let the pending-count decrement
      // below run so the slot is freed. Anything else (e.g. bad_alloc) is a
      // genuine bug and still terminates -- matching emit_event's #2857
      // containment above.
      warn("tx-failure report send failed: {}", e.what());
    }
    pending->fetch_sub(1, std::memory_order_release);
    co_return;
  }(client_, pending_failure_reports_, std::move(tx_hash), std::move(diag));
}

} // namespace hogd
