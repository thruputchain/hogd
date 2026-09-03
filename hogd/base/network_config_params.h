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

// The value in force for a parameter the configuration does not state.
//
// A configuration names only the parameters its version knew about: a v0 chain
// predates some of them outright, and a v1 list runs out before the ones added
// after it was written. These are what a node runs under in that case, which
// makes them consensus-critical in the same way the stated values are -- two
// node versions disagreeing here would disagree about a chain's rules while
// agreeing on its config hash.
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
inline constexpr uint32_t kDefaultMaxNewContractsPerTx = 4;

// The highest parameter this build can name. Adding one means extending this
// alongside the ConfigParamID arm, so promoting a configuration still
// materialises every parameter rather than silently truncating at the old
// highest.
inline constexpr ConfigParamID kHighestConfigParam =
    ConfigParamID::kConfigParamMaxNewContractsPerTx;

namespace detail {

/// The value a v1 list states for `id`, or nullopt when the list omits it.
///
/// A canonical list is the prefix 0..N of the parameter space, so a parameter
/// sits at the index its id names and this is a lookup rather than a search.
/// The discriminant is checked anyway: every list reaching here has been
/// checked for canonical form, but reading the wrong arm of a union is worth
/// one comparison to rule out. A list that fails it is treated as one that ran
/// out early, which is the reading that leaves the caller on defaults rather
/// than on another parameter's value.
inline std::optional<uint32_t>
stated_param(NetworkConfigV1 const &config, ConfigParamID id)
{
  const int32_t index = std::to_underlying(id);
  if (index < 0 || static_cast<size_t>(index) >= config.params.size()) {
    return std::nullopt;
  }

  ConfigParam const &param = config.params[static_cast<size_t>(index)];
  if (param.param() == id) {
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

} // namespace detail

/// Whether a v1 list is in the canonical form the protocol requires.
///
/// Ids must run 0, 1, 2, ... with no gaps, so the list is a prefix of the
/// parameter space. That settles three things at once: the order is fixed, an
/// id cannot repeat, and a parameter cannot be skipped over. The list is
/// hashed as written, so without a single permitted form one configuration
/// would have several hashes; and because the only way to be absent is to be
/// past the end, an absent parameter always means one newer than this
/// configuration rather than one it chose to omit.
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
  default:
    // The union declares no other arm, so an unknown version fails to decode
    // rather than reaching here.
    panic("Unrecoverable; unknown NetworkConfig version: {}", config.v());
  }
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
  default:
    panic("Unrecoverable; unknown NetworkConfig version: {}", config.v());
  }
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
    return detail::stated_param(config.v1(),
                                ConfigParamID::kConfigParamMaxNewContractsPerTx)
        .value_or(kDefaultMaxNewContractsPerTx);
  default:
    panic("Unrecoverable; unknown NetworkConfig version: {}", config.v());
  }
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

namespace detail {

/// The value in force for `id` among already-resolved parameters.
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

/// States `value` for `id`, leaving the list canonical.
///
/// Rebuilds the list as the prefix 0..N, carrying every other parameter's
/// value in force -- a v0 configuration's included, so promotion never drops
/// what it was running under. N covers whatever the configuration already
/// stated and the parameter being set, so a v1 chain stating only a prefix
/// keeps its length rather than gaining parameters it never named. Promoting
/// an older arm instead materialises every parameter, since that arm's values
/// live in fields rather than in the list.
inline void
set_config_param(NetworkConfig &config, ConfigParamID id, uint32_t value)
{
  const NetworkConfigParams params = resolve_config_params(config);

  int32_t highest = std::to_underlying(id);
  if (config.v() == 1) {
    for (auto const &param : config.v1().params) {
      highest = std::max(highest, std::to_underlying(param.param()));
    }
  }
  else {
    // Promotion rewrites the configuration wholesale, so materialise every
    // parameter this build knows rather than a prefix ending at `id`: the
    // older arm states values of its own, and truncating would silently
    // replace one with a default.
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
