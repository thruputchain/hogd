#include "hogd/event/rpc_from_sched.h"

#include "event_rpc.hh"
#include "hogd/base/log.h"
#include "hogd/event/status.h"
#include "rpc_types.hh"
#include "types.hh"
#include "x23/async.h"

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

x23::Async<RPCError>
EventMempoolRpc::eventsched_open_block(BlockID block_id)
{
  info("eventsched_open_block: block_id={}", block_id);
  auto r = co_await ctx_.open_block(block_id);
  if (!r) {
    info("eventsched_open_block: block_id={} error={}", block_id,
         static_cast<int>(r.error().code()));
    co_return to_rpc_error(r.error().code());
  }
  info("eventsched_open_block: block_id={} done", block_id);
  co_return RPCError::kRPCNone;
}

x23::Async<RPCError>
EventMempoolRpc::eventsched_abandon_block(BlockID block_id)
{
  info("eventsched_abandon_block: block_id={}", block_id);
  auto r = co_await ctx_.abandon_block(block_id);
  if (!r) {
    info("eventsched_abandon_block: block_id={} error={}", block_id,
         static_cast<int>(r.error().code()));
    co_return to_rpc_error(r.error().code());
  }
  info("eventsched_abandon_block: block_id={} done", block_id);
  co_return RPCError::kRPCNone;
}

x23::Async<EventBlockResult>
EventMempoolRpc::eventsched_close_block(BlockID block_id,
                                        BlockHeight block_height)
{
  info("eventsched_close_block: block_id={} block_height={}", block_id,
       block_height);
  auto r = co_await ctx_.close_block(block_id, block_height);
  if (!r) {
    info("eventsched_close_block: block_id={} error={}", block_id,
         static_cast<int>(r.error().code()));
    co_return EventBlockResult{to_rpc_error(r.error().code())};
  }
  info("eventsched_close_block: block_id={} done", block_id);
  EventBlockResult result(RPCError::kRPCNone);
  result.root_hash() = r.value();
  co_return result;
}

} // namespace hogd
