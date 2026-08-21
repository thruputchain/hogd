#include "hogd/hog_exec/runtime/event_server_proxy.h"

#include <exception>
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
    // Contain any throw (RPC error or bad_alloc): on this detached coroutine an
    // uncaught throw would terminate the executor (#2857). The diagnostic can
    // itself throw under memory pressure, so guard it too.
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
    // Best-effort: contain any throw so nothing escapes this detached coroutine
    // (#2857), matching emit_event above. The diagnostic is guarded because it
    // too can throw under memory pressure.
    try {
      warn("tx-failure report send failed: {}", e.what());
    } catch (...) {
      report_dropped_log("tx-failure report send failure diagnostic");
    }
  }
  co_return;
}

} // namespace hogd
