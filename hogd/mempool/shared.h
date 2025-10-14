#pragma once

#include <atomic>
#include <chrono>
#include <unordered_map>
#include <utility>

#include "coord_sched.hh"
#include "db_sched.hh"
#include "discovery.hh"
#include "event_rpc.hh"
#include "exec_sched.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "loadgen.hh"
#include "nomination.hh"
#include "rpc_types.hh"
#include "sched_db.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/rpc.h"
#include "x23/vec.h"

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

using sched_db_client_t =
    SchedDb1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

template<typename T>
inline auto
create_client(const std::string &host, uint16_t port)
{
  x23::UniqueFd fd =
      x23::tcpconnect(*x23::HostPort{host.c_str(), port}.to_addr());
  return x23::rpc_stream_logged(default_coropoll(), fd.release())->client(T{});
}

} // namespace hogd
