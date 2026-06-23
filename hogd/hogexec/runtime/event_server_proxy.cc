#include "hogd/hogexec/runtime/event_server_proxy.h"

#include "event.hh"
#include "event_rpc.hh"
#include "ffi_enums.hh"
#include "hogd/base/log.h"
#include "hogd/base/rpc_client.h"
#include "hogd/hogexec/common/status.h"
#include "rpc_types.hh"
#include "x23/async.h"
#include "x23/cpumap.h"
#include "x23/sockio.h"

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
  auto r = co_await client_.eventexecutor_emit_event(block_id, tx_id,
                                                     tx_event_log, proposed_tx);
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
PerTransactionEventServerProxy::emit_events(TxEventLog const &tx_event_log,
                                            ProposedTransaction const &proposed_tx)
{
  if (emitted_event_) {
    co_return SystemStatus<void>{SystemError{SystemErrorCode::Unrecoverable}};
  }
  emitted_event_ = true;
  co_return co_await event_server_proxy_.emit_event(block_id_, tx_id_,
                                                    tx_event_log, proposed_tx);
}

} // namespace hogd
