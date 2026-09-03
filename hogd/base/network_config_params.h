// network_config_params.h - reading the parameters a chain's configuration
// states, whichever version it states them in.

#pragma once

#include <cstdint>
#include <optional>

#include "hogd/base/log.h"
#include "network_config.hh"

namespace hogd {

// The value in force for a parameter the configuration does not state.
//
// A configuration names only the parameters its version knew about: a v0 chain
// predates some of them outright, and a v1 list may omit any of them. These
// are what a node runs under in that case, which makes them consensus-critical
// in the same way the stated values are -- two node versions disagreeing here
// would disagree about a chain's rules while agreeing on its config hash.
//
// Once a chain has run under one of these, it cannot change: doing so would
// silently rewrite the rules that chain has been executing under. A new value
// needs a parameter the chain states outright, not an edit here.
inline constexpr uint32_t kDefaultMaxSHA256Buffers = 8;
inline constexpr uint32_t kDefaultMaxCrossInvokeDepth = 256;

// Chosen to match this repository's stock configuration rather than to be
// permissive: a chain predating the parameter would otherwise attach contracts
// without bound, which is the condition #2190 describes. Deliberately below
// what the executor->storage message cap admits.
inline constexpr uint32_t kDefaultMaxNewContractsPerTx = 32;

namespace detail {

/// The value a v1 list states for `id`, or nullopt when the list omits it.
inline std::optional<uint32_t>
stated_param(NetworkConfigV1 const &config, ConfigParamID id)
{
  for (auto const &param : config.params) {
    if (param.param() != id) {
      continue;
    }
    switch (id) {
    case ConfigParamID::kConfigParamMaxSHA256Buffers:
      return param.max_sha256_buffers();
    case ConfigParamID::kConfigParamMaxCrossInvokeDepth:
      return param.max_cross_invoke_depth();
    case ConfigParamID::kConfigParamMaxNewContractsPerTx:
      return param.max_new_contracts_per_tx();
    }
  }
  return std::nullopt;
}

/// A parameter carrying `value`.
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

/// Replaces the value stated for `id`, or inserts it in id order.
inline void
put_param(NetworkConfigV1 &config, ConfigParamID id, uint32_t value)
{
  for (auto &param : config.params) {
    if (param.param() == id) {
      param = make_param(id, value);
      return;
    }
  }
  NetworkConfigV1 rebuilt;
  bool placed = false;
  for (auto const &param : config.params) {
    if (!placed && id < param.param()) {
      rebuilt.params.push_back(make_param(id, value));
      placed = true;
    }
    rebuilt.params.push_back(param);
  }
  if (!placed) {
    rebuilt.params.push_back(make_param(id, value));
  }
  config = std::move(rebuilt);
}

} // namespace detail

/// Whether a v1 list is in the canonical form the protocol requires.
///
/// Parameters must be strictly ascending by id. The list is hashed into
/// ProposalHeader::config_hash, so two orderings of one set of parameters
/// would be two hashes for a configuration the nodes agree on; strict ascent
/// also rules out a repeated id resolving to whichever copy a node read first.
inline bool
config_params_canonical(NetworkConfigV1 const &config)
{
  std::optional<ConfigParamID> previous;
  for (auto const &param : config.params) {
    if (previous.has_value() && param.param() <= *previous) {
      return false;
    }
    previous = param.param();
  }
  return true;
}

/// Concurrently open incremental SHA-256 buffers permitted per invocation
/// frame.
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
  }
  panic("Unrecoverable; unknown NetworkConfig version: {}", config.v());
}

/// Invocation frames one transaction may have open at once.
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
  }
  panic("Unrecoverable; unknown NetworkConfig version: {}", config.v());
}

/// Contracts one transaction may attach in `contracts_to_deploy`.
inline uint32_t
max_new_contracts_per_tx(NetworkConfig const &config)
{
  switch (config.v()) {
  case 0:
    // v0 predates the parameter, so such a chain runs under the default.
    return kDefaultMaxNewContractsPerTx;
  case 1:
    return detail::stated_param(
               config.v1(), ConfigParamID::kConfigParamMaxNewContractsPerTx)
        .value_or(kDefaultMaxNewContractsPerTx);
  }
  panic("Unrecoverable; unknown NetworkConfig version: {}", config.v());
}

/// States `value` for `id`, leaving the list canonical.
///
/// A v0 configuration is promoted to v1 first, carrying forward the values it
/// was running under -- including the ones it never stated -- so setting one
/// parameter never quietly moves another.
inline void
set_config_param(NetworkConfig &config, ConfigParamID id, uint32_t value)
{
  if (config.v() != 1) {
    const uint32_t buffers = max_sha256_buffers(config);
    const uint32_t depth = max_cross_invoke_depth(config);
    const uint32_t contracts = max_new_contracts_per_tx(config);
    config.v(1);
    detail::put_param(config.v1(),
                      ConfigParamID::kConfigParamMaxSHA256Buffers, buffers);
    detail::put_param(config.v1(),
                      ConfigParamID::kConfigParamMaxCrossInvokeDepth, depth);
    detail::put_param(config.v1(),
                      ConfigParamID::kConfigParamMaxNewContractsPerTx,
                      contracts);
  }
  detail::put_param(config.v1(), id, value);
}

/// The parameters in force, resolved from whatever version a chain states them
/// in.
///
/// Resolve once where a tip is pinned and read fields from this, rather than
/// asking the configuration per access: a v1 chain states its parameters as a
/// list, and the invoke-depth and hash-buffer bounds are checked on paths hot
/// enough that walking one per check would show.
struct NetworkConfigParams {
  uint32_t max_sha256_buffers;
  uint32_t max_cross_invoke_depth;
  uint32_t max_new_contracts_per_tx;
};

/// The parameters `config` puts in force, flattened.
inline NetworkConfigParams
resolve_config_params(NetworkConfig const &config)
{
  return NetworkConfigParams{
      .max_sha256_buffers = max_sha256_buffers(config),
      .max_cross_invoke_depth = max_cross_invoke_depth(config),
      .max_new_contracts_per_tx = max_new_contracts_per_tx(config),
  };
}

} // namespace hogd
