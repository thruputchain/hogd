// network_config_params.h - reading the parameters a chain's configuration
// states.

#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
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
// critical: changing a default also changes the rules for configurations that
// omit that parameter.
inline constexpr uint32_t kDefaultMaxSHA256Buffers = 32;
inline constexpr uint32_t kDefaultMaxCrossInvokeDepth = 16;

// Not permissive: a chain predating the parameter would otherwise attach
// contracts without bound, which is #2190.
inline constexpr uint32_t kDefaultMaxNewContractsPerTx = 4;

// Unbounded before these parameters, so a contract-sized span was parsed or
// copied before the call could be rejected (#4174, #4040). The calldata
// default matches guest memory, so it bounds a root invocation's calldata
// without failing any sub-invoke that runs today.
inline constexpr uint32_t kDefaultMaxAuthTokens = 64;
inline constexpr uint32_t kDefaultMaxCalldataLen = 65536;

// The value the wire type fixed before the parameter existed, so a chain that
// does not state one keeps exactly the limit it already ran under.
inline constexpr uint32_t kDefaultMaxReturnBufferLen = 1024;

// Not permissive: native-token availability was the only bound on a
// transaction's budget before this parameter, and a chain that does not state
// one now gets this cap rather than none.
inline constexpr GasAmount kDefaultMaxTxGas = 1'000'000'000;

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
  uint32_t max_auth_tokens = kDefaultMaxAuthTokens;
  uint32_t max_calldata_len = kDefaultMaxCalldataLen;
  uint32_t max_return_buffer_len = kDefaultMaxReturnBufferLen;
  GasAmount max_tx_gas = kDefaultMaxTxGas;

  // The defaults in force: what an empty parameter list resolves to.
  NetworkConfigParams() = default;

  bool operator==(NetworkConfigParams const &) const = default;

  // The parameters `config` puts in force, flattened.
  explicit NetworkConfigParams(NetworkConfigV0 const &config);

  // Whether the list is canonical: ids run 0, 1, 2, ... with no gaps. Hashed
  // as written, so one form per configuration is one hash per configuration.
  static bool is_canonical(NetworkConfigV0 const &config);

  // States `value` for `id`, rebuilding the list as a canonical prefix that
  // carries every other parameter's value in force.
  static void set_param(NetworkConfig &config, ConfigParamID id,
                        uint64_t value);

  // A ConfigParam stating `value` for `id`.
  // `value` must fit the arm's type: a uint32 arm CHECKs rather than
  // narrowing, which would state a value the caller never asked for.
  static ConfigParam make_param(ConfigParamID id, uint64_t value);

  // A canonical configuration stating every parameter this build knows at its
  // default, so the defaults above are the only place those values live.
  static NetworkConfig stated_defaults();

  // A complete reading of the values execution uses, including defaults.
  NetworkConfig to_network_config() const;

private:
  // The value the list states for `id`, or nullopt past its end. Canonical
  // form puts it at its own index, so a mismatch is a bug: CHECK, don't hide
  // it.
  static std::optional<uint64_t> stated_param(NetworkConfigV0 const &config,
                                              ConfigParamID id);

  // Narrows a uint32 arm's value, which its arm type guarantees fits.
  static uint32_t to_u32(uint64_t value);

  uint64_t in_force(ConfigParamID id) const;
};

// The highest parameter id this build knows. A configuration states its ids
// contiguously from zero, so this bounds a scan across all of them.
inline constexpr ConfigParamID kHighestConfigParamID =
    ConfigParamID::kConfigParamMaxTxGas;

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
  case ConfigParamID::kConfigParamMaxAuthTokens:
    return "max_auth_tokens";
  case ConfigParamID::kConfigParamMaxCalldataLen:
    return "max_calldata_len";
  case ConfigParamID::kConfigParamMaxReturnBufferLen:
    return "max_return_buffer_len";
  case ConfigParamID::kConfigParamMaxTxGas:
    return "max_tx_gas";
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

// The largest value `id`'s arm can carry. Most arms are `unsigned int`; a
// value past this cannot be stated for `id` at all.
inline uint64_t
config_param_max_value(ConfigParamID id)
{
  switch (id) {
  case ConfigParamID::kConfigParamMaxTxGas:
    return std::numeric_limits<GasAmount>::max();
  case ConfigParamID::kConfigParamMaxSHA256Buffers:
  case ConfigParamID::kConfigParamMaxCrossInvokeDepth:
  case ConfigParamID::kConfigParamMaxNewContractsPerTx:
  case ConfigParamID::kConfigParamMaxBytestringLen:
  case ConfigParamID::kConfigParamMaxContractSizeBytes:
  case ConfigParamID::kConfigParamMaxSavepointDepth:
  case ConfigParamID::kConfigParamMaxSHA256BufferBytes:
  case ConfigParamID::kConfigParamMaxAuthTokenDepth:
  case ConfigParamID::kConfigParamMaxRv32CodeBytes:
  case ConfigParamID::kConfigParamMaxRv32MemoryBytes:
  case ConfigParamID::kConfigParamMaxAuthTokens:
  case ConfigParamID::kConfigParamMaxCalldataLen:
  case ConfigParamID::kConfigParamMaxReturnBufferLen:
    return std::numeric_limits<uint32_t>::max();
  }
  panic("Unrecoverable; unknown ConfigParamID: {}", std::to_underlying(id));
}

// The value `param` states, read from the arm it names.
inline uint64_t
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
  case ConfigParamID::kConfigParamMaxAuthTokens:
    return param.max_auth_tokens();
  case ConfigParamID::kConfigParamMaxCalldataLen:
    return param.max_calldata_len();
  case ConfigParamID::kConfigParamMaxReturnBufferLen:
    return param.max_return_buffer_len();
  case ConfigParamID::kConfigParamMaxTxGas:
    return param.max_tx_gas();
  }
  panic("Unrecoverable; unknown ConfigParamID: {}",
        std::to_underlying(param.param()));
}

inline std::optional<uint64_t>
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

inline uint32_t
NetworkConfigParams::to_u32(uint64_t value)
{
  CHECK_LE(value, std::numeric_limits<uint32_t>::max());
  return static_cast<uint32_t>(value);
}

inline ConfigParam
NetworkConfigParams::make_param(ConfigParamID id, uint64_t value)
{
  ConfigParam out(id);
  switch (id) {
  case ConfigParamID::kConfigParamMaxSHA256Buffers:
    out.max_sha256_buffers() = to_u32(value);
    break;
  case ConfigParamID::kConfigParamMaxCrossInvokeDepth:
    out.max_cross_invoke_depth() = to_u32(value);
    break;
  case ConfigParamID::kConfigParamMaxNewContractsPerTx:
    out.max_new_contracts_per_tx() = to_u32(value);
    break;
  case ConfigParamID::kConfigParamMaxBytestringLen:
    out.max_bytestring_len() = to_u32(value);
    break;
  case ConfigParamID::kConfigParamMaxContractSizeBytes:
    out.max_contract_size_bytes() = to_u32(value);
    break;
  case ConfigParamID::kConfigParamMaxSavepointDepth:
    out.max_savepoint_depth() = to_u32(value);
    break;
  case ConfigParamID::kConfigParamMaxSHA256BufferBytes:
    out.max_sha256_buffer_bytes() = to_u32(value);
    break;
  case ConfigParamID::kConfigParamMaxAuthTokenDepth:
    out.max_auth_token_depth() = to_u32(value);
    break;
  case ConfigParamID::kConfigParamMaxRv32CodeBytes:
    out.max_rv32_code_bytes() = to_u32(value);
    break;
  case ConfigParamID::kConfigParamMaxRv32MemoryBytes:
    out.max_rv32_memory_bytes() = to_u32(value);
    break;
  case ConfigParamID::kConfigParamMaxAuthTokens:
    out.max_auth_tokens() = to_u32(value);
    break;
  case ConfigParamID::kConfigParamMaxCalldataLen:
    out.max_calldata_len() = to_u32(value);
    break;
  case ConfigParamID::kConfigParamMaxReturnBufferLen:
    out.max_return_buffer_len() = to_u32(value);
    break;
  case ConfigParamID::kConfigParamMaxTxGas:
    out.max_tx_gas() = value;
    break;
  }
  return out;
}

inline uint64_t
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
  case ConfigParamID::kConfigParamMaxAuthTokens:
    return max_auth_tokens;
  case ConfigParamID::kConfigParamMaxCalldataLen:
    return max_calldata_len;
  case ConfigParamID::kConfigParamMaxReturnBufferLen:
    return max_return_buffer_len;
  case ConfigParamID::kConfigParamMaxTxGas:
    return max_tx_gas;
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
        to_u32(stated_param(config, ConfigParamID::kConfigParamMaxSHA256Buffers)
                   .value_or(kDefaultMaxSHA256Buffers))),
    max_cross_invoke_depth(to_u32(
        stated_param(config, ConfigParamID::kConfigParamMaxCrossInvokeDepth)
            .value_or(kDefaultMaxCrossInvokeDepth))),
    max_new_contracts_per_tx(to_u32(
        stated_param(config, ConfigParamID::kConfigParamMaxNewContractsPerTx)
            .value_or(kDefaultMaxNewContractsPerTx))),
    max_bytestring_len(
        to_u32(stated_param(config, ConfigParamID::kConfigParamMaxBytestringLen)
                   .value_or(kDefaultMaxBytestringLen))),
    max_contract_size_bytes(to_u32(
        stated_param(config, ConfigParamID::kConfigParamMaxContractSizeBytes)
            .value_or(kDefaultMaxContractSizeBytes))),
    max_savepoint_depth(to_u32(
        stated_param(config, ConfigParamID::kConfigParamMaxSavepointDepth)
            .value_or(kMaxSavepointDepth))),
    max_sha256_buffer_bytes(to_u32(
        stated_param(config, ConfigParamID::kConfigParamMaxSHA256BufferBytes)
            .value_or(kSHA256BufferMaxSize))),
    max_auth_token_depth(to_u32(
        stated_param(config, ConfigParamID::kConfigParamMaxAuthTokenDepth)
            .value_or(kAuthTokenMaxDepth))),
    max_rv32_code_bytes(
        to_u32(stated_param(config, ConfigParamID::kConfigParamMaxRv32CodeBytes)
                   .value_or(kDefaultMaxRv32CodeBytes))),
    max_rv32_memory_bytes(to_u32(
        stated_param(config, ConfigParamID::kConfigParamMaxRv32MemoryBytes)
            .value_or(kDefaultMaxRv32MemoryBytes))),
    max_auth_tokens(
        to_u32(stated_param(config, ConfigParamID::kConfigParamMaxAuthTokens)
                   .value_or(kDefaultMaxAuthTokens))),
    max_calldata_len(
        to_u32(stated_param(config, ConfigParamID::kConfigParamMaxCalldataLen)
                   .value_or(kDefaultMaxCalldataLen))),
    max_return_buffer_len(to_u32(
        stated_param(config, ConfigParamID::kConfigParamMaxReturnBufferLen)
            .value_or(kDefaultMaxReturnBufferLen))),
    max_tx_gas(stated_param(config, ConfigParamID::kConfigParamMaxTxGas)
                   .value_or(kDefaultMaxTxGas))
{}

inline void
NetworkConfigParams::set_param(NetworkConfig &config, ConfigParamID id,
                               uint64_t value)
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

inline NetworkConfig
NetworkConfigParams::to_network_config() const
{
  NetworkConfig config;
  config.v(0);
  for (int32_t i = 0; i <= std::to_underlying(kHighestConfigParamID); ++i) {
    const auto id = static_cast<ConfigParamID>(i);
    config.v0().params.push_back(make_param(id, in_force(id)));
  }
  return config;
}

inline NetworkConfig
NetworkConfigParams::stated_defaults()
{
  // set_param fills every lower id with the value in force, so stating the
  // highest id on an empty list states all of them.
  NetworkConfig config;
  config.v(0);
  set_param(config, kHighestConfigParamID,
            NetworkConfigParams{}.in_force(kHighestConfigParamID));
  return config;
}

} // namespace hogd
