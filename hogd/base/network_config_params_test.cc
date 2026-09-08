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
  NetworkConfigParams::set_param(
      config, ConfigParamID::kConfigParamMaxCrossInvokeDepth, depth);
  return config;
}

} // namespace

// An unstated depth resolves to the stock default, which is at or below the
// ceiling and so is used unchanged.
TEST(NetworkConfigParamsTest, UnsetCrossInvokeDepthIsStockDefault)
{
  NetworkConfig config;
  config.v(0);
  const NetworkConfigParams params(config.v0());
  EXPECT_EQ(params.max_cross_invoke_depth, kDefaultMaxCrossInvokeDepth);
}

// A configured depth below the ceiling is honored exactly: the parameter can
// tighten the bound.
TEST(NetworkConfigParamsTest, CrossInvokeDepthBelowCeilingIsHonored)
{
  const NetworkConfig config = config_with_cross_invoke_depth(64);
  const NetworkConfigParams params(config.v0());
  EXPECT_EQ(params.max_cross_invoke_depth, 64U);
}

// The ceiling itself is admissible and used unchanged.
TEST(NetworkConfigParamsTest, CrossInvokeDepthAtCeilingIsHonored)
{
  const NetworkConfig config =
      config_with_cross_invoke_depth(kMaxCrossInvokeDepth);
  const NetworkConfigParams params(config.v0());
  EXPECT_EQ(params.max_cross_invoke_depth, kMaxCrossInvokeDepth);
}

// A depth above the ceiling -- only reachable via corruption or on-disk
// tampering of the DBTIP, which is outside the consensus state hash (issue
// #3919) -- is clamped so the executor keeps an independent hard maximum on
// live invocation frames.
TEST(NetworkConfigParamsTest, CrossInvokeDepthAboveCeilingIsClamped)
{
  const NetworkConfig config =
      config_with_cross_invoke_depth(std::numeric_limits<uint32_t>::max());
  const NetworkConfigParams params(config.v0());
  EXPECT_EQ(params.max_cross_invoke_depth, kMaxCrossInvokeDepth);
}

} // namespace hogd
