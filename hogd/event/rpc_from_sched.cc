#include "hogd/event/rpc_from_sched.h"

#include "event_rpc.hh"
#include "hogd/base/log.h"
#include "hogd/event/status.h"
#include "rpc_types.hh"

namespace hogd {

namespace {

RPCError
to_rpc_error(EventErrorCode error_code)
{
  switch (error_code) {
  case EventErrorCode::kNone:
    return RPCError::kRPCNone;
  case EventErrorCode::kExists:
  case EventErrorCode::kBadBlockID:
    return RPCError::kRPCBadBlockID;
  case EventErrorCode::kInternal:
    return RPCError::kRPCUnrecoverable;
  }
  error("Invalid event error code: {}", static_cast<int>(error_code));
  return RPCError::kRPCUnrecoverable;
}
} // namespace

RPCError
EventSchedRpc::eventsched_open_block(BlockID block_id)
{
  auto r = ctx_.open_block(block_id);
  if (!r) {
    return to_rpc_error(r.error().code());
  }
  return RPCError::kRPCNone;
}

RPCError
EventSchedRpc::eventsched_abandon_block(BlockID block_id)
{
  auto r = ctx_.abandon_block(block_id);
  if (!r) {
    return to_rpc_error(r.error().code());
  }
  return RPCError::kRPCNone;
}

EventBlockResult
EventSchedRpc::eventsched_close_block(BlockID block_id)
{
  info("eventsched_close_block");
  auto r = co_await ctx_.close_block(block_id);
  info("eventsched_close_block");
  if (!r) {
    info("eventsched_close_block");
    return EventBlockResult{to_rpc_error(r.error().code())};
  }
  info("eventsched_close_block");
  EventBlockResult result(RPCError::kRPCNone);
  info("eventsched_close_block");
  result.root_hash() = r.value();
  info("eventsched_close_block");
  return result;
}

} // namespace hogd
