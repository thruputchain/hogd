#pragma once

#include <cstddef>

namespace hogd {

// Message-size cap for internal service-to-service RPC that forwards bulk
// database state (e.g. HogDb <-> auth / mempool key-change and scheduling
// batches). A single committed block can exceed the 16 MiB x23 default, so
// these links negotiate a much larger cap. Kept separate from loadgen's own
// cap so production DB/auth/mempool endpoints don't depend on a loadgen
// artifact.
//
// Capped at the ONC RPC single-fragment record limit (0x7fffffff): x23's
// make_record() emits one fragment, so a larger message cannot be framed and
// would truncate its record marker. If bulk state ever needs to exceed this,
// x23 must learn to emit multi-fragment records first.
inline constexpr size_t kBulkRpcMaxMessageSize = 0x7fffffff;
static_assert(kBulkRpcMaxMessageSize <= 0x7fffffffULL,
              "message-size cap must fit a single ONC RPC record fragment");

} // namespace hogd
