// cross_invoke_limit.h - the compiled ceiling on simultaneous invocation
// frames, enforced by the executor independently of the network config.
#pragma once

#include <algorithm>
#include <cstdint>

#include "hogd/base/network_config_params.h"

namespace hogd {

// A compiled ceiling on how many invocation frames one transaction may keep
// open. Every open frame holds a live contract runtime, so bounding the frame
// count bounds the executor's live-runtime memory to a small, recoverable
// multiple of the stock footprint no matter what value the config states. A
// chain may lower the frame limit but never raise it above this ceiling. That
// matters because the config is read off the DBTIP, which sits outside the
// consensus state hash and so can be rewritten on disk without perturbing
// storage_long_hash (hogd issue #3919); the clamp keeps such a rewrite (or
// plain corruption) from disabling the bound and letting one transaction hold
// unbounded live frames until the executor faults. Set generously above the
// stock default (kDefaultMaxCrossInvokeDepth) so no legitimate configuration is
// affected.
inline constexpr uint32_t kMaxCrossInvokeDepth = 1024;
static_assert(kMaxCrossInvokeDepth >= kDefaultMaxCrossInvokeDepth);

// The number of simultaneously-open invocation frames the executor admits under
// a tip stating `configured` max_cross_invoke_depth: the stated value clamped
// to the compiled ceiling.
constexpr uint32_t
cross_invoke_frame_limit(uint32_t configured)
{
  return std::min(configured, kMaxCrossInvokeDepth);
}

} // namespace hogd
