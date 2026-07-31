#pragma once

#include <cstddef>

namespace hogd {

// Message-size cap for internal service-to-service RPC that forwards bulk
// database state (e.g. HogDB <-> auth / mempool key-change and scheduling
// batches). A single committed block can exceed the 16 MiB x23 default, so
// these links negotiate a much larger cap. Kept separate from loadgen's own
// cap so production DB/auth/mempool endpoints don't depend on a loadgen
// artifact.
inline constexpr size_t kBulkRpcMaxMessageSize = 0x7fffffffff;

// EventSched1 carries the full proposal payload so event can durably retain a
// pre-commit candidate. Keep this cap scoped to that internal RPC surface.
inline constexpr size_t kEventSchedulerRpcMaxMessageSize = 0x7fffffff;

} // namespace hogd
