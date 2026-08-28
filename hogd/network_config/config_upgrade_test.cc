#include <gtest/gtest.h>

#include <cstdint>

#include "hogd/network_config/config_upgrade.h"
#include "network_config.hh"
#include "types.hh"

namespace hogd {

namespace {

constexpr uint32_t kBuffers = 8;
constexpr uint32_t kDepth = 64;

NetworkConfig
base_config()
{
  return NetworkConfig{
      .ext = ExtensionPoint(0),
      .max_sha256_buffers = kBuffers,
      .max_cross_invoke_depth = kDepth,
  };
}

ConfigUpgrade
upgrade_of(ConfigParamID id, uint32_t value)
{
  ConfigUpgrade upgrade(id);
  switch (id) {
  case ConfigParamID::kConfigParamMaxSHA256Buffers:
    upgrade.max_sha256_buffers() = value;
    break;
  case ConfigParamID::kConfigParamMaxCrossInvokeDepth:
    upgrade.max_cross_invoke_depth() = value;
    break;
  }
  return upgrade;
}

} // namespace

TEST(ConfigUpgradeTest, ChangesOnlyTheParamItNames)
{
  const NetworkConfig before = base_config();

  const NetworkConfig buffers_changed = apply_config_upgrade(
      before, upgrade_of(ConfigParamID::kConfigParamMaxSHA256Buffers, 2));
  EXPECT_EQ(buffers_changed.max_sha256_buffers, 2u);
  EXPECT_EQ(buffers_changed.max_cross_invoke_depth, kDepth);

  const NetworkConfig depth_changed = apply_config_upgrade(
      before, upgrade_of(ConfigParamID::kConfigParamMaxCrossInvokeDepth, 5));
  EXPECT_EQ(depth_changed.max_cross_invoke_depth, 5u);
  EXPECT_EQ(depth_changed.max_sha256_buffers, kBuffers);
}

// A block changes several parameters by carrying one upgrade per parameter,
// so applying them in sequence has to accumulate rather than overwrite.
TEST(ConfigUpgradeTest, UpgradesAccumulate)
{
  NetworkConfig config = base_config();
  config = apply_config_upgrade(
      config, upgrade_of(ConfigParamID::kConfigParamMaxSHA256Buffers, 3));
  config = apply_config_upgrade(
      config, upgrade_of(ConfigParamID::kConfigParamMaxCrossInvokeDepth, 7));

  EXPECT_EQ(config.max_sha256_buffers, 3u);
  EXPECT_EQ(config.max_cross_invoke_depth, 7u);
}

TEST(ConfigUpgradeTest, LeavesTheInputAlone)
{
  const NetworkConfig before = base_config();
  auto after = apply_config_upgrade(
      before, upgrade_of(ConfigParamID::kConfigParamMaxSHA256Buffers, 1));

  EXPECT_EQ(before.max_sha256_buffers, kBuffers);
  EXPECT_NE(after, before);
}

} // namespace hogd
