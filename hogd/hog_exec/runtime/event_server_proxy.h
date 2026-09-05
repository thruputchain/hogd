#pragma once

#include <utility>

#include "db_conflict.hh"
#include "event.hh"
#include "event_rpc.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "rpc_types.hh"
#include "sched_event.hh"
#include "transaction.hh"
#include "x23/async.h"
#include "x23/cpumap.h"
#include "x23/format.h"
#include "x23/rpc.h"
#include "x23/sockio.h"
#include "x23/srpc.h"

namespace hogd {

class EventServerProxy {
public:
  // max_message_size caps the client-side RPC streams only (tests lower it to
  // provoke oversized-send failures without mutating the process-wide
  // x23::default_max_message_size, which racing server threads also read).
  EventServerProxy(x23::HostPort const &host_port,
                   x23::CpuMap const &map = *x23::llc_map,
                   size_t max_message_size = x23::default_max_message_size);

  static EventServerProxy make_from_exec_result(EventCard const &event)
  {
    // TODO(geoff): validate the other fields in event card.
    info("Connecting to event server at {}", x23::show(event));
    x23::HostPort host_port;
    if (!host_port.parse(event.address_for_exec)) {
      panic("Failed to parse event server address");
    }
    return EventServerProxy(host_port);
  }

  x23::Async<SystemStatus<void>> open_block(BlockID block_id);
  x23::Async<SystemStatus<void>> close_block(BlockID block_id);

  x23::Async<SystemStatus<void>>
  emit_event(BlockID block_id, TransactionID tx_id,
             TxEventLog const &tx_event_log,
             ProposedTransaction const &proposed_tx);

  // Best-effort report that [tx_hash] failed nondeterministically during 2pc,
  // with the diagnostic hog_db attached. Awaitable; callers fire-and-forget it
  // (see report_db_conflict_diagnostic, which spawns it detached the same way
  // ExecutorContext spawns emit_events). The coroutine copies client_ up front
  // (client_t wraps a refcounted ClientRef), so a spawned send is
  // self-contained and can safely outlive this proxy; a slow or dead event
  // server costs nothing but the dropped report -- RPC errors are logged and
  // swallowed, because a diagnostic must never disturb execution.
  x23::Async<void> report_tx_failure(TransactionHash tx_hash,
                                     TxFailureDiagnostic diag);

private:
  using client_t =
      EventExecutor1::client<x23::RpcMulticlient<x23::RpcLogger>::ClientRef>;
  client_t client_;
};

class PerTransactionEventServerProxy {
public:
  PerTransactionEventServerProxy(EventServerProxy &event_server_proxy,
                                 BlockID block_id, TransactionID tx_id);

  // Delete copy constructor and assignment
  PerTransactionEventServerProxy(PerTransactionEventServerProxy const &) =
      delete;
  PerTransactionEventServerProxy &
  operator=(PerTransactionEventServerProxy const &) = delete;
  // allow move ctor
  PerTransactionEventServerProxy(PerTransactionEventServerProxy &&) = default;
  PerTransactionEventServerProxy &
  operator=(PerTransactionEventServerProxy &&) = delete;

  x23::Async<SystemStatus<void>>
  emit_events(TxEventLog const &tx_event_log,
              ProposedTransaction const &proposed_tx);

  // Awaitable; see EventServerProxy::report_tx_failure. Callers
  // fire-and-forget.
  x23::Async<void> report_tx_failure(TransactionHash tx_hash,
                                     TxFailureDiagnostic diag) const
  {
    return event_server_proxy_.report_tx_failure(std::move(tx_hash),
                                                 std::move(diag));
  }

private:
  EventServerProxy &event_server_proxy_;
  const BlockID block_id_;
  const TransactionID tx_id_;
  bool emitted_event_ = false;
};

} // namespace hogd
