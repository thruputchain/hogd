#include <gtest/gtest.h>

#include <limits>

#include "hogd/base/network_config_params.h"
#include "network_config.hh"

namespace hogd {
namespace {

// A canonical config that states `depth` for max_cross_invoke_depth and leaves
// every other parameter at its in-force default.
NetworkConfig
config_with_cross_invoke_depth(uint32_t depth)
{
  NetworkConfig config;
  config.v(0);
  NetworkConfigParams::set_param(config,
                                 ConfigParamID::kConfigParamMaxCrossInvokeDepth,
                                 depth);
  return config;
}

} // namespace

// An unstated depth resolves to the stock default, which is at or below the
// ceiling and so is enforced unchanged.
TEST(NetworkConfigParamsTest, UnsetCrossInvokeDepthIsStockDefault)
{
  NetworkConfig config;
  config.v(0);
  const NetworkConfigParams params(config.v0());
  EXPECT_EQ(params.max_cross_invoke_depth, kDefaultMaxCrossInvokeDepth);
  EXPECT_EQ(params.effective_max_cross_invoke_depth(),
            kDefaultMaxCrossInvokeDepth);
}

// A configured depth below the ceiling is honored exactly: the parameter can
// tighten the bound.
TEST(NetworkConfigParamsTest, CrossInvokeDepthBelowCeilingIsHonored)
{
  const NetworkConfig config = config_with_cross_invoke_depth(64);
  const NetworkConfigParams params(config.v0());
  EXPECT_EQ(params.max_cross_invoke_depth, 64U);
  EXPECT_EQ(params.effective_max_cross_invoke_depth(), 64U);
}

// The ceiling itself is admissible and enforced unchanged.
TEST(NetworkConfigParamsTest, CrossInvokeDepthAtCeilingIsHonored)
{
  const NetworkConfig config =
      config_with_cross_invoke_depth(kMaxCrossInvokeDepth);
  const NetworkConfigParams params(config.v0());
  EXPECT_EQ(params.max_cross_invoke_depth, kMaxCrossInvokeDepth);
  EXPECT_EQ(params.effective_max_cross_invoke_depth(), kMaxCrossInvokeDepth);
}

// A depth above the ceiling -- only reachable via corruption or on-disk
// tampering of the DBTIP, which is outside the consensus state hash (issue
// #3919) -- leaves the stated field faithful but is clamped when resolved for
// execution, so the executor keeps an independent hard maximum on live frames.
TEST(NetworkConfigParamsTest, CrossInvokeDepthAboveCeilingIsClampedForExecution)
{
  const NetworkConfig config =
      config_with_cross_invoke_depth(std::numeric_limits<uint32_t>::max());
  const NetworkConfigParams params(config.v0());
  EXPECT_EQ(params.max_cross_invoke_depth,
            std::numeric_limits<uint32_t>::max());
  EXPECT_EQ(params.effective_max_cross_invoke_depth(), kMaxCrossInvokeDepth);
}

// The clamp must not leak into set_param's rebuild of untouched fields, or a
// depth above the ceiling would be re-serialized as the clamped value and the
// resulting raw config (hence its config_hash) would depend on the order the
// builder set parameters. Setting another parameter after a high depth must
// leave that depth intact.
TEST(NetworkConfigParamsTest, SetParamPreservesDepthAboveCeiling)
{
  constexpr uint32_t kHighDepth = kMaxCrossInvokeDepth + 1000;
  NetworkConfig config = config_with_cross_invoke_depth(kHighDepth);
  NetworkConfigParams::set_param(config,
                                 ConfigParamID::kConfigParamMaxSHA256Buffers,
                                 3);
  const NetworkConfigParams params(config.v0());
  EXPECT_EQ(params.max_cross_invoke_depth, kHighDepth);
  EXPECT_EQ(params.max_sha256_buffers, 3U);
}

} // namespace hogd
