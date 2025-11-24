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

// Inclusive-exclusive half-open range used when slicing transaction batches.
struct ChunkRange {
  size_t start;
  size_t end; // [start, end)
};

inline std::vector<ChunkRange>
make_chunks(size_t total_size, size_t chunk_count)
{
  std::vector<ChunkRange> chunks;
  if (!total_size || !chunk_count) {
    return chunks;
  }
  chunk_count = std::min(chunk_count, total_size);

  const size_t base_size = total_size / chunk_count;
  const size_t remainder = total_size % chunk_count;

  size_t offset = 0;
  chunks.reserve(chunk_count);
  for (size_t i = 0; i < chunk_count; ++i) {
    // Distribute remainder across the first [remainder] chunks to keep them as
    // balanced as possible.
    const size_t chunk_size = base_size + (i < remainder ? 1 : 0);
    const size_t start = offset;
    const size_t end = start + chunk_size;
    chunks.push_back({start, end});
    offset = end;
  }
  CHECK_EQ(offset, total_size);

  return chunks;
}

inline std::vector<ChunkRange>
make_chunks_for_max_size(size_t total_size, size_t max_chunk_size)
{
  if (!max_chunk_size) {
    return {};
  }
  // Round up so we never exceed [max_chunk_size] in any bucket.
  const size_t chunk_count = (total_size + max_chunk_size - 1) / max_chunk_size;
  return make_chunks(total_size, chunk_count);
}

} // namespace hogd
