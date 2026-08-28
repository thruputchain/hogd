#include "hogd/network_config/config_upgrade.h"

#include "hogd/base/log.h"
#include "network_config.hh"

namespace hogd {

NetworkConfig
apply_config_upgrade(NetworkConfig const &config, ConfigUpgrade const &upgrade)
{
  NetworkConfig out = config;
  switch (upgrade.param()) {
  case ConfigParamID::kConfigParamMaxSHA256Buffers:
    out.max_sha256_buffers = upgrade.max_sha256_buffers();
    return out;
  case ConfigParamID::kConfigParamMaxCrossInvokeDepth:
    out.max_cross_invoke_depth = upgrade.max_cross_invoke_depth();
    return out;
  }
  panic("bug: unhandled ConfigParamID in apply_config_upgrade: {}",
        upgrade.param());
}

} // namespace hogd
