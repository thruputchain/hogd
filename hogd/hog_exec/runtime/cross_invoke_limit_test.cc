#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

#include "hogd/base/network_config_params.h"
#include "hogd/hog_exec/runtime/cross_invoke_limit.h"

namespace hogd {

// A configured depth at or below the ceiling is admitted as stated: the
// parameter can tighten the frame bound.
TEST(CrossInvokeFrameLimitTest, AtOrBelowCeilingIsHonored)
{
  EXPECT_EQ(cross_invoke_frame_limit(1), 1U);
  EXPECT_EQ(cross_invoke_frame_limit(kDefaultMaxCrossInvokeDepth),
            kDefaultMaxCrossInvokeDepth);
  EXPECT_EQ(cross_invoke_frame_limit(kMaxCrossInvokeDepth),
            kMaxCrossInvokeDepth);
}

// A configured depth above the ceiling -- only reachable via corruption or
// on-disk tampering of the DBTIP, which is outside the consensus state hash
// (hogd issue #3919) -- is clamped to the ceiling, so the executor keeps an
// independent hard maximum on simultaneous live frames. This is what fails if
// the clamp is inverted or the ceiling is set too high.
TEST(CrossInvokeFrameLimitTest, AboveCeilingIsClamped)
{
  EXPECT_EQ(cross_invoke_frame_limit(kMaxCrossInvokeDepth + 1),
            kMaxCrossInvokeDepth);
  EXPECT_EQ(cross_invoke_frame_limit(std::numeric_limits<uint32_t>::max()),
            kMaxCrossInvokeDepth);
}

} // namespace hogd
