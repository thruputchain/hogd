#pragma once

#include <stdexcept>

#include "event.hh"
#include "event_rpc.hh"
#include "hogd/base/log.h"
#include "hogd/hogexec/common/status.h"
#include "rpc_types.hh"
#include "sched_event.hh"
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

  x23::Async<SystemStatus<void>> emit_events(TxEventLog const &tx_event_log,
                                             ProposedTransaction const &proposed_tx);

private:
  EventServerProxy &event_server_proxy_;
  const BlockID block_id_;
  const TransactionID tx_id_;
  bool emitted_event_ = false;
};

} // namespace hogd
