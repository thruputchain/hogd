#pragma once

#include <atomic>
#include <chrono>
#include <unordered_map>
#include <utility>

#include "coord_sched.hh"
#include "db_coord.hh"
#include "discovery.hh"
#include "exec_coord.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "nomination.hh"
#include "rpc_types.hh"
#include "sched_coord.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/rpc.h"
#include "x23/vec.h"

namespace hogd {

using coord_client_t =
    CoordScheduler1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

using db_client_t = DbCoord1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

using exec_client_t =
    ExecutorCoord1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

} // namespace hogd
