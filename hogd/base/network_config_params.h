// network_config_params.h - reading the parameters a chain's configuration
// states.

#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

#include "auth_token.hh"
#include "contract.hh"
#include "hog_exec_constants.hh"
#include "hogd/base/log.h"
#include "network_config.hh"
#include "storage_item.hh"

namespace hogd {

// Values in force for parameters a configuration does not state. Consensus-
// critical, so frozen once any chain has run under them.
inline constexpr uint32_t kDefaultMaxSHA256Buffers = 32;
inline constexpr uint32_t kDefaultMaxCrossInvokeDepth = 16;

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
  uint32_t max_contract_size_bytes =
      static_cast<uint32_t>(kDefaultMaxContractSizeBytes);
  uint32_t max_savepoint_depth = static_cast<uint32_t>(kMaxSavepointDepth);
  uint32_t max_sha256_buffer_bytes =
      static_cast<uint32_t>(kSHA256BufferMaxSize);
  uint32_t max_auth_token_depth = static_cast<uint32_t>(kAuthTokenMaxDepth);
  uint32_t max_rv32_code_bytes =
      static_cast<uint32_t>(kDefaultMaxRv32CodeBytes);
  uint32_t max_rv32_memory_bytes =
      static_cast<uint32_t>(kDefaultMaxRv32MemoryBytes);
  uint32_t max_nondeterministic_bits =
      static_cast<uint32_t>(kDefaultMaxNondeterministicBits);

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

// The highest parameter id this build knows. A configuration states its ids
// contiguously from zero, so this bounds a scan across all of them.
inline constexpr ConfigParamID kHighestConfigParamID =
    ConfigParamID::kConfigParamMaxNondeterministicBits;

// Wire name for `id`: --network-config, recorded overrides, genesis JSON.
inline std::string_view
config_param_name(ConfigParamID id)
{
  switch (id) {
  case ConfigParamID::kConfigParamMaxSHA256Buffers:
    return "max_sha256_buffers";
  case ConfigParamID::kConfigParamMaxCrossInvokeDepth:
    return "max_cross_invoke_depth";
  case ConfigParamID::kConfigParamMaxNewContractsPerTx:
    return "max_new_contracts_per_tx";
  case ConfigParamID::kConfigParamMaxBytestringLen:
    return "max_bytestring_len";
  case ConfigParamID::kConfigParamMaxContractSizeBytes:
    return "max_contract_size_bytes";
  case ConfigParamID::kConfigParamMaxSavepointDepth:
    return "max_savepoint_depth";
  case ConfigParamID::kConfigParamMaxSHA256BufferBytes:
    return "max_sha256_buffer_bytes";
  case ConfigParamID::kConfigParamMaxAuthTokenDepth:
    return "max_auth_token_depth";
  case ConfigParamID::kConfigParamMaxRv32CodeBytes:
    return "max_rv32_code_bytes";
  case ConfigParamID::kConfigParamMaxRv32MemoryBytes:
    return "max_rv32_memory_bytes";
  case ConfigParamID::kConfigParamMaxNondeterministicBits:
    return "max_nondeterministic_bits";
  }
  panic("Unrecoverable; unknown ConfigParamID: {}", std::to_underlying(id));
}

// The parameter `name` names, or nullopt when no parameter carries it.
// Callers that reject a name decide what an unknown one means.
inline std::optional<ConfigParamID>
config_param_id(std::string_view name)
{
  const int32_t highest = std::to_underlying(kHighestConfigParamID);
  for (int32_t i = 0; i <= highest; ++i) {
    const auto id = static_cast<ConfigParamID>(i);
    if (config_param_name(id) == name) {
      return id;
    }
  }
  return std::nullopt;
}

// The value `param` states, read from the arm it names.
inline uint32_t
config_param_value(ConfigParam const &param)
{
  switch (param.param()) {
  case ConfigParamID::kConfigParamMaxSHA256Buffers:
    return param.max_sha256_buffers();
  case ConfigParamID::kConfigParamMaxCrossInvokeDepth:
    return param.max_cross_invoke_depth();
  case ConfigParamID::kConfigParamMaxNewContractsPerTx:
    return param.max_new_contracts_per_tx();
  case ConfigParamID::kConfigParamMaxBytestringLen:
    return param.max_bytestring_len();
  case ConfigParamID::kConfigParamMaxContractSizeBytes:
    return param.max_contract_size_bytes();
  case ConfigParamID::kConfigParamMaxSavepointDepth:
    return param.max_savepoint_depth();
  case ConfigParamID::kConfigParamMaxSHA256BufferBytes:
    return param.max_sha256_buffer_bytes();
  case ConfigParamID::kConfigParamMaxAuthTokenDepth:
    return param.max_auth_token_depth();
  case ConfigParamID::kConfigParamMaxRv32CodeBytes:
    return param.max_rv32_code_bytes();
  case ConfigParamID::kConfigParamMaxRv32MemoryBytes:
    return param.max_rv32_memory_bytes();
  case ConfigParamID::kConfigParamMaxNondeterministicBits:
    return param.max_nondeterministic_bits();
  }
  panic("Unrecoverable; unknown ConfigParamID: {}",
        std::to_underlying(param.param()));
}

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
  return config_param_value(param);
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
  case ConfigParamID::kConfigParamMaxContractSizeBytes:
    out.max_contract_size_bytes() = value;
    break;
  case ConfigParamID::kConfigParamMaxSavepointDepth:
    out.max_savepoint_depth() = value;
    break;
  case ConfigParamID::kConfigParamMaxSHA256BufferBytes:
    out.max_sha256_buffer_bytes() = value;
    break;
  case ConfigParamID::kConfigParamMaxAuthTokenDepth:
    out.max_auth_token_depth() = value;
    break;
  case ConfigParamID::kConfigParamMaxRv32CodeBytes:
    out.max_rv32_code_bytes() = value;
    break;
  case ConfigParamID::kConfigParamMaxRv32MemoryBytes:
    out.max_rv32_memory_bytes() = value;
    break;
  case ConfigParamID::kConfigParamMaxNondeterministicBits:
    out.max_nondeterministic_bits() = value;
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
  case ConfigParamID::kConfigParamMaxContractSizeBytes:
    return max_contract_size_bytes;
  case ConfigParamID::kConfigParamMaxSavepointDepth:
    return max_savepoint_depth;
  case ConfigParamID::kConfigParamMaxSHA256BufferBytes:
    return max_sha256_buffer_bytes;
  case ConfigParamID::kConfigParamMaxAuthTokenDepth:
    return max_auth_token_depth;
  case ConfigParamID::kConfigParamMaxRv32CodeBytes:
    return max_rv32_code_bytes;
  case ConfigParamID::kConfigParamMaxRv32MemoryBytes:
    return max_rv32_memory_bytes;
  case ConfigParamID::kConfigParamMaxNondeterministicBits:
    return max_nondeterministic_bits;
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
            .value_or(static_cast<uint32_t>(kDefaultMaxBytestringLen))),
    max_contract_size_bytes(
        stated_param(config, ConfigParamID::kConfigParamMaxContractSizeBytes)
            .value_or(static_cast<uint32_t>(kDefaultMaxContractSizeBytes))),
    max_savepoint_depth(
        stated_param(config, ConfigParamID::kConfigParamMaxSavepointDepth)
            .value_or(static_cast<uint32_t>(kMaxSavepointDepth))),
    max_sha256_buffer_bytes(
        stated_param(config, ConfigParamID::kConfigParamMaxSHA256BufferBytes)
            .value_or(static_cast<uint32_t>(kSHA256BufferMaxSize))),
    max_auth_token_depth(
        stated_param(config, ConfigParamID::kConfigParamMaxAuthTokenDepth)
            .value_or(static_cast<uint32_t>(kAuthTokenMaxDepth))),
    max_rv32_code_bytes(
        stated_param(config, ConfigParamID::kConfigParamMaxRv32CodeBytes)
            .value_or(static_cast<uint32_t>(kDefaultMaxRv32CodeBytes))),
    max_rv32_memory_bytes(
        stated_param(config, ConfigParamID::kConfigParamMaxRv32MemoryBytes)
            .value_or(static_cast<uint32_t>(kDefaultMaxRv32MemoryBytes))),
    max_nondeterministic_bits(
        stated_param(config, ConfigParamID::kConfigParamMaxNondeterministicBits)
            .value_or(static_cast<uint32_t>(kDefaultMaxNondeterministicBits)))
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
