// network_config_params.h - reading the parameters a chain's configuration
// states.

#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>

#include "hogd/base/log.h"
#include "network_config.hh"
#include "storage_item.hh"

namespace hogd {

// Values in force for parameters a configuration does not state. Consensus-
// critical, so frozen once any chain has run under them.
inline constexpr uint32_t kDefaultMaxSHA256Buffers = 8;
inline constexpr uint32_t kDefaultMaxCrossInvokeDepth = 256;

// Not permissive: a chain predating the parameter would otherwise attach
// contracts without bound, which is #2190.
inline constexpr uint32_t kDefaultMaxNewContractsPerTx = 4;

// The parameters in force, flattened from the list that states them.
// Resolve once where a tip is pinned; the invoke path reads these per frame.
struct NetworkConfigParams {
  uint32_t max_sha256_buffers = kDefaultMaxSHA256Buffers;
  uint32_t max_cross_invoke_depth = kDefaultMaxCrossInvokeDepth;
  uint32_t max_new_contracts_per_tx = kDefaultMaxNewContractsPerTx;
  uint32_t max_bytestring_len = static_cast<uint32_t>(kDefaultMaxBytestringLen);

  // The defaults in force: what an empty parameter list resolves to.
  NetworkConfigParams() = default;

  // The parameters `config` puts in force, flattened.
  explicit NetworkConfigParams(NetworkConfigV0 const &config);

  // Whether the list is canonical: ids run 0, 1, 2, ... with no gaps. Hashed
  // as written, so one form per configuration is one hash per configuration.
  static bool is_canonical(NetworkConfigV0 const &config);

  // States `value` for `id`, rebuilding the list as a canonical prefix that
  // carries every other parameter's value in force.
  static void set_param(NetworkConfig &config, ConfigParamID id,
                        uint32_t value);

  // A ConfigParam stating `value` for `id`.
  static ConfigParam make_param(ConfigParamID id, uint32_t value);

private:
  // The value the list states for `id`, or nullopt past its end. Canonical
  // form puts it at its own index, so a mismatch is a bug: CHECK, don't hide
  // it.
  static std::optional<uint32_t> stated_param(NetworkConfigV0 const &config,
                                              ConfigParamID id);

  uint32_t in_force(ConfigParamID id) const;
};

inline std::optional<uint32_t>
NetworkConfigParams::stated_param(NetworkConfigV0 const &config,
                                  ConfigParamID id)
{
  const int32_t index = std::to_underlying(id);
  CHECK_GE(index, 0);

  if (std::cmp_greater_equal(index, config.params.size())) {
    return std::nullopt;
  }

  ConfigParam const &param = config.params[static_cast<size_t>(index)];
  CHECK_EQ(std::to_underlying(param.param()), index);

  switch (id) {
  case ConfigParamID::kConfigParamMaxSHA256Buffers:
    return param.max_sha256_buffers();
  case ConfigParamID::kConfigParamMaxCrossInvokeDepth:
    return param.max_cross_invoke_depth();
  case ConfigParamID::kConfigParamMaxNewContractsPerTx:
    return param.max_new_contracts_per_tx();
  case ConfigParamID::kConfigParamMaxBytestringLen:
    return param.max_bytestring_len();
  }
  panic("Unrecoverable; unknown ConfigParamID: {}", index);
}

inline ConfigParam
NetworkConfigParams::make_param(ConfigParamID id, uint32_t value)
{
  ConfigParam out(id);
  switch (id) {
  case ConfigParamID::kConfigParamMaxSHA256Buffers:
    out.max_sha256_buffers() = value;
    break;
  case ConfigParamID::kConfigParamMaxCrossInvokeDepth:
    out.max_cross_invoke_depth() = value;
    break;
  case ConfigParamID::kConfigParamMaxNewContractsPerTx:
    out.max_new_contracts_per_tx() = value;
    break;
  case ConfigParamID::kConfigParamMaxBytestringLen:
    out.max_bytestring_len() = value;
    break;
  }
  return out;
}

inline uint32_t
NetworkConfigParams::in_force(ConfigParamID id) const
{
  switch (id) {
  case ConfigParamID::kConfigParamMaxSHA256Buffers:
    return max_sha256_buffers;
  case ConfigParamID::kConfigParamMaxCrossInvokeDepth:
    return max_cross_invoke_depth;
  case ConfigParamID::kConfigParamMaxNewContractsPerTx:
    return max_new_contracts_per_tx;
  case ConfigParamID::kConfigParamMaxBytestringLen:
    return max_bytestring_len;
  }
  panic("Unrecoverable; unknown ConfigParamID: {}", std::to_underlying(id));
}

inline bool
NetworkConfigParams::is_canonical(NetworkConfigV0 const &config)
{
  int32_t expected = 0;
  for (auto const &param : config.params) {
    if (std::to_underlying(param.param()) != expected) {
      return false;
    }
    ++expected;
  }
  return true;
}

inline NetworkConfigParams::NetworkConfigParams(NetworkConfigV0 const &config)
  : max_sha256_buffers(
        stated_param(config, ConfigParamID::kConfigParamMaxSHA256Buffers)
            .value_or(kDefaultMaxSHA256Buffers)),
    max_cross_invoke_depth(
        stated_param(config, ConfigParamID::kConfigParamMaxCrossInvokeDepth)
            .value_or(kDefaultMaxCrossInvokeDepth)),
    max_new_contracts_per_tx(
        stated_param(config, ConfigParamID::kConfigParamMaxNewContractsPerTx)
            .value_or(kDefaultMaxNewContractsPerTx)),
    max_bytestring_len(
        stated_param(config, ConfigParamID::kConfigParamMaxBytestringLen)
            .value_or(static_cast<uint32_t>(kDefaultMaxBytestringLen)))
{}

inline void
NetworkConfigParams::set_param(NetworkConfig &config, ConfigParamID id,
                               uint32_t value)
{
  const NetworkConfigParams params(config.v0());

  // The ctor above CHECKs canonical form, so the ids are exactly 0..size-1
  // and the last one is the highest stated.
  const int32_t highest =
      std::max(std::to_underlying(id),
               static_cast<int32_t>(config.v0().params.size()) - 1);

  NetworkConfig rebuilt;
  rebuilt.v(0);
  for (int32_t i = 0; i <= highest; ++i) {
    const auto param_id = static_cast<ConfigParamID>(i);
    rebuilt.v0().params.push_back(
        make_param(param_id,
                   param_id == id ? value : params.in_force(param_id)));
  }
  config = std::move(rebuilt);
}

} // namespace hogd
