#include "hogd/hogexec/runtime/event_server_proxy.h"

#include <format>
#include <stdexcept>
#include <utility>

#include "event.hh"
#include "event_rpc.hh"
#include "hogd/hogexec/runtime/status.h"
#include "rpc_types.hh"
#include "x23/sockio.h"
#include "x23/srpc.h"

namespace hogd {

namespace {

auto
connect_to_event_server(x23::HostPort host_port)
{

  auto server_addr = host_port.to_addr(true);
  if (!server_addr) {
    throw std::runtime_error(
        std::format("failed server error: {}", server_addr.error()));
  }
  return x23::SRpcMulticlient<x23::RpcLogger>::make(server_addr.value())
      ->client(EventExecutor1{});
}

} // namespace

EventServerProxy::EventServerProxy(x23::HostPort host_port)
  : client_(connect_to_event_server(std::move(host_port)))
{
  info("Making event server proxy from host port {}", host_port.show());
}

PerTransactionEventServerProxy::PerTransactionEventServerProxy(
    EventServerProxy &event_server_proxy, BlockID block_id, TransactionID tx_id)
  : event_server_proxy_(event_server_proxy), block_id_(block_id), tx_id_(tx_id)
{}

SystemStatus<void>
EventServerProxy::emit_event(BlockID block_id, TransactionID tx_id,
                             TxEventLog const &tx_event_log)
{
  info("Emitting event for block {} and tx {}", block_id, tx_id);
  auto r = client_.eventexecutor_emit_event(block_id, tx_id, tx_event_log);
  switch (r) {
  case RPCError::kRPCNone:
    return {};
  case RPCError::kRPCBadBlockID:
    return SystemError{SystemErrorCode::kAbortTransaction};
  default:
    return SystemError{SystemErrorCode::kUnrecoverable};
  }
}

SystemStatus<void>
PerTransactionEventServerProxy::emit_events(TxEventLog const &tx_event_log)
{
  if (emitted_event_) {
    return SystemError{SystemErrorCode::kUnrecoverable};
  }
  emitted_event_ = true;
  return event_server_proxy_.emit_event(block_id_, tx_id_, tx_event_log);
}

} // namespace hogd
