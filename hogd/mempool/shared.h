#pragma once

#include <cstdint>
#include <string>
#include <sys/socket.h>
#include <vector>

#include "auth_rpc.hh"
#include "block.hh"
#include "coord_sched.hh"
#include "db_sched.hh"
#include "event_rpc.hh"
#include "exec_sched.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/time.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/metrics/metrics.h"
#include "loadgen.hh"
#include "parallel.hh"
#include "sched_coord.hh"
#include "sched_db.hh"
#include "transaction.hh"
#include "x23/async.h"
#include "x23/rpc.h"
#include "x23/serialize.h"
#include "x23/sockio.h"
#include "x23/vec.h"

namespace hogd {

using auth_sched_client_t =
    AuthMempool1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

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

inline SHA256Hash
generate_block_hash(const BlockHeader &header)
{
  return sha256(header);
}

inline Metrics::Gauge mempool_current_block_height_metric;

inline void
init_metrics()
{
  mempool_current_block_height_metric = Metrics::instance().gauge(
      Metrics::ComponentName::kMempool, "current_block_height",
      "The height of the last block committed by the mempool.");
  Metrics::instance().mark_done_with_init(Metrics::ComponentName::kMempool);
}

inline void
record_current_block_height_metric(BlockHeight height)
{
  mempool_current_block_height_metric.set(height);
}

} // namespace hogd
