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

// EventSched1 carries the full proposal payload so event can durably retain a
// pre-commit candidate. Keep this cap scoped to that internal RPC surface.
inline constexpr size_t kEventSchedulerRpcMaxMessageSize = 0x7fffffff;

// Message-size cap for read-only query surfaces served to external
// consumers (the indexer block-explorer server). Query requests and
// responses are small; keeping the cap at the 16 MiB x23 default bounds
// what a misbehaving client can make the server buffer, unlike the bulk
// intra-node links above.
inline constexpr size_t kQueryRpcMaxMessageSize = 0x100'0000;

// Per-request batch-size cap for the gateway-facing RPC surfaces
// (DbGateway1::dbgateway_get_items, SchedGateway1::schedgateway_lookup_tx).
// The production JSON-RPC gateway's adaptive batcher coalesces up to 1000
// storage reads into one dbgateway_get_items call by default, so the cap
// sits just above that while still bounding what an arbitrary client can
// make the node process in a single request.
inline constexpr size_t kMaxGatewayBatchSize = 1024;

} // namespace hogd
