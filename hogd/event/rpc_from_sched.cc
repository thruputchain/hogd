#include "hogd/event/rpc_from_sched.h"

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

} // namespace hogd
