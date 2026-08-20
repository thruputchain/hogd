#include "hogd/hog_exec/runtime/event_server_proxy.h"

#include <utility>

#include "db_conflict.hh"
#include "event.hh"
#include "event_rpc.hh"
#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "x23/async.h"
#include "x23/cpumap.h"
#include "x23/sockio.h"
#include "x23/vec.h"

namespace hogd {

namespace {

auto
connect_to_event_server(x23::HostPort const &host_port, x23::CpuMap const &map,
                        size_t max_message_size)
{
  info("Connecting to event server on host port {}", host_port.show());
  return make_rpc_multiclient<EventExecutor1>(host_port, true, map,
                                              max_message_size);
}

} // namespace

EventServerProxy::EventServerProxy(x23::HostPort const &host_port,
                                   x23::CpuMap const &map,
                                   size_t max_message_size)
  : client_(connect_to_event_server(host_port, map, max_message_size))
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
  } catch (std::exception const &e) {
    // The RPC client can throw instead of returning a status; on this detached
    // event coroutine an uncaught throw would terminate the executor (#2857).
    // Contain every escaping throw here and return a structured Unrecoverable
    // result instead. This covers:
    //   - x23::XdrBadInput   -- client-side, over-cap payload
    //   - x23::XdrCallFailed -- failed call
    //   - std::bad_alloc     -- local OOM, e.g. while serializing the request
    //                           via x23::make_record (which runs before the
    //                           first co_await in send_call, so its throw
    //                           propagates out of the co_await above).
    // The diagnostic itself allocates while formatting and can throw again
    // under the same memory pressure, so guard it and fall back to the
    // no-allocation reporter (cf. database_proxy.cc commit_transaction).
    try {
      error("Unrecoverable: event emission threw before an RPC status: {}",
            e.what());
    } catch (...) {
      report_dropped_log("event emission failure diagnostic");
    }
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

x23::Async<void>
EventServerProxy::report_tx_failure(TransactionHash tx_hash,
                                    TxFailureDiagnostic diag)
{
  // Copy the client up front (before the first co_await, so it happens while
  // the caller still owns the proxy): a fire-and-forget send then owns its own
  // refcounted ClientRef and can safely outlive this proxy. Unlike
  // emit_events, nothing drains these at finish_block, so the send must not
  // depend on `this`.
  client_t client = client_;
  try {
    RPCError r =
        co_await client.eventexecutor_report_tx_failure(std::move(tx_hash),
                                                        std::move(diag));
    if (r != RPCError::kRPCNone) {
      warn("tx-failure report dropped by event server: {}", r);
    }
  } catch (std::exception const &e) {
    // Best-effort: the RPC client can throw instead of returning a status;
    // swallow every escaping throw here so nothing escapes into the detached
    // coroutine and terminates the executor (#2857). Matches emit_event's
    // containment above; std::bad_alloc from x23::make_record serialization
    // is included, and the diagnostic is guarded because warn() itself
    // allocates while formatting and can throw again under memory pressure.
    try {
      warn("tx-failure report send failed: {}", e.what());
    } catch (...) {
      report_dropped_log("tx-failure report send failure diagnostic");
    }
  }
  co_return;
}

} // namespace hogd
