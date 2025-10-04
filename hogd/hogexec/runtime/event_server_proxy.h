#pragma once

#include "event.hh"
#include "event_rpc.hh"
#include "hogd/hogexec/runtime/status.h"
#include "rpc_types.hh"
#include "x23/sockio.h"
#include "x23/srpc.h"

namespace hogd {

class EventServerProxy {
public:
  EventServerProxy(x23::HostPort host_port);

  // Delete copy and move constructor and assignment
  EventServerProxy(EventServerProxy const &) = delete;
  EventServerProxy &operator=(EventServerProxy const &) = delete;
  EventServerProxy(EventServerProxy &&) = delete;
  EventServerProxy &operator=(EventServerProxy &&) = delete;

  SystemStatus<void> emit_event(BlockID block_id, TransactionID tx_id,
                                TxEventLog const &tx_event_log);

private:
  using client_t =
      ExecutorEvent1::client<x23::SRpcMulticlient<x23::RpcLogger>::ClientRef>;
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

  SystemStatus<void> emit_events(TxEventLog const &tx_event_log);

private:
  EventServerProxy &event_server_proxy_;
  const BlockID block_id_;
  const TransactionID tx_id_;
  bool emitted_event_ = false;
};

} // namespace hogd
