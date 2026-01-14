#pragma once

#include <string>
#include <sys/socket.h>

#include "coord_sched.hh"
#include "db_sched.hh"
#include "event_rpc.hh"
#include "exec_sched.hh"
#include "hogd/base/log.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "loadgen.hh"
#include "sched_coord.hh"
#include "sched_db.hh"
#include "x23/rpc.h"
#include "x23/sockio.h"

namespace hogd {

using coord_sched_client_t =
    CoordScheduler1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

using db_sched_client_t =
    DbSched1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

using event_sched_client_t =
    EventSched1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

using exec_sched_client_t =
    ExecutorSched1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

using loadgen_client_t =
    LoadgenInterfaceV1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

using sched_coord_client_t =
    SchedulerCoord1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

using sched_db_client_t =
    SchedDb1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

template<typename T>
inline auto
create_client(const sockaddr_storage &ss, bool retry = false,
              size_t max_message_size = x23::default_max_message_size)
{
  return make_rpc_client<T>(ss, retry, max_message_size);
}

template<typename T>
inline auto
create_client(x23::HostPort &host_port, bool retry = false,
              size_t max_message_size = x23::default_max_message_size)
{
  auto ss_or_error = host_port.to_addr(/*dns=*/true);
  CHECK(ss_or_error.has_value());
  return create_client<T>(*ss_or_error, retry, max_message_size);
}

template<typename T>
inline auto
create_client(const std::string &host, uint16_t port, bool retry = false,
              size_t max_message_size = x23::default_max_message_size)
{
  x23::HostPort host_port{host.c_str(), port};
  return create_client<T>(host_port, retry, max_message_size);
}

template<typename T>
inline bool
parallel_batch_to_t(const ParallelBatch &parallel, x23::XdrVec<T> &objs)
{
  CHECK_EQ(parallel.segment_tokens.size(), parallel.segment_bytes.size());

  std::vector<size_t> byte_offsets;
  byte_offsets.reserve(parallel.segment_bytes.size());
  size_t total_segment_bytes = 0;
  for (unsigned int segment_bytes : parallel.segment_bytes) {
    byte_offsets.push_back(total_segment_bytes);
    total_segment_bytes += segment_bytes;
  }
  CHECK_EQ(total_segment_bytes, parallel.xdr.size());

  size_t total_segment_tokens = 0;
  for (unsigned int segment_tokens : parallel.segment_tokens) {
    total_segment_tokens += segment_tokens;
  }
  objs.resize(total_segment_tokens, {});

  size_t objs_offset = 0;
  // TODO(jhumphri): Each iteration of this for loop should run in a separate
  // thread.
  for (size_t i = 0; i < parallel.segment_tokens.size(); i++) {
    const unsigned int num_segment_tokens = parallel.segment_tokens[i];
    const unsigned int num_segment_bytes = parallel.segment_bytes[i];

    size_t inner_xdr_offset = 0;
    for (size_t j = 0; j < num_segment_tokens; j++) {
      try {
        size_t num_consumed = x23::deserialize_from(
            &objs[objs_offset++],
            x23::BufView(&parallel.xdr[byte_offsets[i] + inner_xdr_offset],
                         num_segment_bytes - inner_xdr_offset));
        inner_xdr_offset += num_consumed;
      } catch (const x23::XdrBadInput &e) {
        info("failed to deserialize: {}", e.what());
        return false;
      }
    }
  }
  return true;
}

template<typename T>
inline void
t_to_parallel_batch(const x23::XdrVec<T> &objs, ParallelBatch &parallel)
{
  parallel.xdr = x23::serialize(objs);
  parallel.segment_tokens = {static_cast<unsigned int>(objs.size())};
  parallel.segment_bytes = {static_cast<unsigned int>(parallel.xdr.size())};
}

template<typename T>
inline ParallelBatch
t_to_parallel_batch(const x23::XdrVec<T> &objs)
{
  ParallelBatch parallel;
  t_to_parallel_batch(objs, parallel);
  return parallel;
}

} // namespace hogd
