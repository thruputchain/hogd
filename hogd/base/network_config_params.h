// network_config_params.h - reading the parameters a chain's configuration
// states, whichever version it states them in.

#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>

#include "hogd/base/log.h"
#include "network_config.hh"

namespace hogd {

// Values in force for parameters a configuration does not state. Consensus-
// critical, so frozen once any chain has run under them.
inline constexpr uint32_t kDefaultMaxSHA256Buffers = 8;
inline constexpr uint32_t kDefaultMaxCrossInvokeDepth = 256;

// Not permissive: a chain predating the parameter would otherwise attach
// contracts without bound, which is #2190.
inline constexpr uint32_t kDefaultMaxNewContractsPerTx = 4;

// The highest parameter this build can name; extend it with every new
// ConfigParamID or promotion silently truncates at the old highest.
inline constexpr ConfigParamID kHighestConfigParam =
    ConfigParamID::kConfigParamMaxNewContractsPerTx;

namespace detail {

// The value a v1 list states for `id`, or nullopt past its end. Canonical form
// puts it at its own index, so a mismatch is a bug: CHECK, don't hide it.
inline std::optional<uint32_t>
stated_param(NetworkConfigV1 const &config, ConfigParamID id)
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
  }
  panic("Unrecoverable; unknown ConfigParamID: {}", index);
}

inline ConfigParam
make_param(ConfigParamID id, uint32_t value)
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
  }
  return out;
}

} // namespace detail

// Whether a v1 list is canonical: ids run 0, 1, 2, ... with no gaps. Hashed as
// written, so one form per configuration is one hash per configuration.
inline bool
config_params_canonical(NetworkConfigV1 const &config)
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

// Concurrently open incremental SHA-256 buffers permitted per invocation
// frame.
inline uint32_t
max_sha256_buffers(NetworkConfig const &config)
{
  switch (config.v()) {
  case 0:
    return config.v0().max_sha256_buffers;
  case 1:
    return detail::stated_param(config.v1(),
                                ConfigParamID::kConfigParamMaxSHA256Buffers)
        .value_or(kDefaultMaxSHA256Buffers);
  default:
    // The union declares no other arm, so an unknown version fails to decode
    // rather than reaching here.
    panic("Unrecoverable; unknown NetworkConfig version: {}", config.v());
  }
}

// Invocation frames one transaction may have open at once.
inline uint32_t
max_cross_invoke_depth(NetworkConfig const &config)
{
  switch (config.v()) {
  case 0:
    return config.v0().max_cross_invoke_depth;
  case 1:
    return detail::stated_param(config.v1(),
                                ConfigParamID::kConfigParamMaxCrossInvokeDepth)
        .value_or(kDefaultMaxCrossInvokeDepth);
  default:
    panic("Unrecoverable; unknown NetworkConfig version: {}", config.v());
  }
}

// Contracts one transaction may attach in `contracts_to_deploy`.
inline uint32_t
max_new_contracts_per_tx(NetworkConfig const &config)
{
  switch (config.v()) {
  case 0:
    // v0 predates the parameter, so such a chain runs under the default.
    return kDefaultMaxNewContractsPerTx;
  case 1:
    return detail::stated_param(config.v1(),
                                ConfigParamID::kConfigParamMaxNewContractsPerTx)
        .value_or(kDefaultMaxNewContractsPerTx);
  default:
    panic("Unrecoverable; unknown NetworkConfig version: {}", config.v());
  }
}

// The parameters in force, flattened from whichever version states them.
// Resolve once where a tip is pinned; the invoke path reads these per frame.
struct NetworkConfigParams {
  uint32_t max_sha256_buffers;
  uint32_t max_cross_invoke_depth;
  uint32_t max_new_contracts_per_tx;
};

// The parameters `config` puts in force, flattened.
inline NetworkConfigParams
resolve_config_params(NetworkConfig const &config)
{
  return NetworkConfigParams{
      .max_sha256_buffers = max_sha256_buffers(config),
      .max_cross_invoke_depth = max_cross_invoke_depth(config),
      .max_new_contracts_per_tx = max_new_contracts_per_tx(config),
  };
}

namespace detail {

inline uint32_t
in_force(NetworkConfigParams const &params, ConfigParamID id)
{
  switch (id) {
  case ConfigParamID::kConfigParamMaxSHA256Buffers:
    return params.max_sha256_buffers;
  case ConfigParamID::kConfigParamMaxCrossInvokeDepth:
    return params.max_cross_invoke_depth;
  case ConfigParamID::kConfigParamMaxNewContractsPerTx:
    return params.max_new_contracts_per_tx;
  }
  panic("Unrecoverable; unknown ConfigParamID: {}", std::to_underlying(id));
}

} // namespace detail

// States `value` for `id`, rebuilding the list as a canonical prefix that
// carries every other parameter's value in force.
inline void
set_config_param(NetworkConfig &config, ConfigParamID id, uint32_t value)
{
  const NetworkConfigParams params = resolve_config_params(config);

  int32_t highest = std::to_underlying(id);
  if (config.v() == 1) {
    // resolve_config_params above CHECKs canonical form, so the ids are
    // exactly 0..size-1 and the last one is the highest stated.
    highest =
        std::max(highest, static_cast<int32_t>(config.v1().params.size()) - 1);
  }
  else {
    // Materialise every parameter: truncating at `id` would replace a stated
    // v0 value with a default.
    highest = std::max(highest, std::to_underlying(kHighestConfigParam));
  }

  NetworkConfig rebuilt;
  rebuilt.v(1);
  for (int32_t i = 0; i <= highest; ++i) {
    const auto param_id = static_cast<ConfigParamID>(i);
    rebuilt.v1().params.push_back(detail::make_param(
        param_id, param_id == id ? value : detail::in_force(params, param_id)));
  }
  config = std::move(rebuilt);
}

} // namespace hogd
