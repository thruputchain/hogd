#pragma once

#include <atomic>
#include <memory>
#include <stdexcept>
#include <utility>

#include "db_conflict.hh"
#include "event.hh"
#include "event_rpc.hh"
#include "hogd/base/log.h"
#include "hogd/hogexec/common/status.h"
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
  EventServerProxy(x23::HostPort const &host_port,
                   x23::CpuMap const &map = *x23::llc_map);

  EventServerProxy(EventServerProxy const &) = delete;
  EventServerProxy &operator=(EventServerProxy const &) = delete;
  EventServerProxy(EventServerProxy &&) = default;

  static EventServerProxy make_from_exec_result(EventCard const &event)
  {
    // TODO(geoff): validate the other fields in event card.
    info("Connecting to event server at {}", x23::show(event));
    x23::HostPort host_port;
    if (!host_port.parse(event.address_for_exec)) {
      throw std::runtime_error("Failed to parse event server address");
    }
    return EventServerProxy(host_port);
  }

  x23::Async<SystemStatus<void>> open_block(BlockID block_id);
  x23::Async<SystemStatus<void>> close_block(BlockID block_id);

  x23::Async<SystemStatus<void>>
  emit_event(BlockID block_id, TransactionID tx_id,
             TxEventLog const &tx_event_log,
             ProposedTransaction const &proposed_tx);

  // Fire-and-forget, best-effort report that [tx_hash] failed
  // nondeterministically during 2pc, with the diagnostic hogdb attached.
  // Returns immediately; a slow or dead event server costs nothing but the
  // dropped report (errors are logged and swallowed -- diagnostics must
  // never disturb execution). The detached report coroutine owns a
  // refcounted copy of client_ (client_t wraps a refcounted ClientRef, cf.
  // x23::RpcMulticlient::ClientRef) and a shared_ptr to the pending-count
  // atomic, so it never dereferences this EventServerProxy and destroying
  // the proxy can never race an in-flight send. In-flight sends are capped
  // at kMaxPendingFailureReports; excess reports are dropped, so a conflict
  // storm against a slow event server cannot grow memory.
  void report_tx_failure(SignedTransactionHash tx_hash,
                         TxFailureDiagnostic diag);

private:
  using client_t =
      EventExecutor1::client<x23::RpcMulticlient<x23::RpcLogger>::ClientRef>;
  client_t client_;

  // Soft bound on concurrently in-flight failure reports (racy over-admission
  // by a few is fine); reports past the cap are dropped, keeping the
  // fire-and-forget path memory-bounded even with an unresponsive server.
  // Held via shared_ptr so the detached report coroutine can keep its own
  // refcounted handle on the counter after this proxy is destroyed.
  static constexpr ssize_t kMaxPendingFailureReports = 1024;
  std::shared_ptr<std::atomic<ssize_t>> pending_failure_reports_ =
      std::make_shared<std::atomic<ssize_t>>(0);
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

  // Fire-and-forget; see EventServerProxy::report_tx_failure.
  void report_tx_failure(SignedTransactionHash tx_hash,
                         TxFailureDiagnostic diag) const
  {
    event_server_proxy_.report_tx_failure(std::move(tx_hash), std::move(diag));
  }

private:
  EventServerProxy &event_server_proxy_;
  const BlockID block_id_;
  const TransactionID tx_id_;
  bool emitted_event_ = false;
};

} // namespace hogd
