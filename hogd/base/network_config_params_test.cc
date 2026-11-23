#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

#include "hog_exec_constants.hh"
#include "hogd/base/network_config_params.h"
#include "network_config.hh"
#include "syscall.hh"
#include "types.hh"

namespace hogd {

// Wire names, spelled out so a rename has to be made here too.
constexpr std::array<std::pair<ConfigParamID, std::string_view>, 19> kNames{{
    {ConfigParamID::kConfigParamMaxSHA256Buffers, "max_sha256_buffers"},
    {ConfigParamID::kConfigParamMaxCrossInvokeDepth, "max_cross_invoke_depth"},
    {ConfigParamID::kConfigParamMaxNewContractsPerTx,
     "max_new_contracts_per_tx"},
    {ConfigParamID::kConfigParamMaxBytestringLen, "max_bytestring_len"},
    {ConfigParamID::kConfigParamMaxContractSizeBytes,
     "max_contract_size_bytes"},
    {ConfigParamID::kConfigParamMaxSavepointDepth, "max_savepoint_depth"},
    {ConfigParamID::kConfigParamMaxSHA256BufferBytes,
     "max_sha256_buffer_bytes"},
    {ConfigParamID::kConfigParamMaxAuthTokenDepth, "max_auth_token_depth"},
    {ConfigParamID::kConfigParamMaxRv32CodeBytes, "max_rv32_code_bytes"},
    {ConfigParamID::kConfigParamMaxRv32MemoryBytes, "max_rv32_memory_bytes"},
    {ConfigParamID::kConfigParamMaxAuthTokens, "max_auth_tokens"},
    {ConfigParamID::kConfigParamMaxCalldataLen, "max_calldata_len"},
    {ConfigParamID::kConfigParamMaxReturnBufferLen, "max_return_buffer_len"},
    {ConfigParamID::kConfigParamMaxTxGas, "max_tx_gas"},
    {ConfigParamID::kConfigParamMinRv32ArtifactVersion,
     "min_rv32_artifact_version"},
    {ConfigParamID::kConfigParamMaxRv32ArtifactVersion,
     "max_rv32_artifact_version"},
    {ConfigParamID::kConfigParamMaxSubtransactionOutputBytes,
     "max_subtransaction_output_bytes"},
    {ConfigParamID::kConfigParamMinSyscallApiMinor, "min_syscall_api_minor"},
    {ConfigParamID::kConfigParamMaxSyscallApiMinor, "max_syscall_api_minor"},
}};

TEST(ConfigParamNameTest, EveryParameterCarriesItsWireName)
{
  for (auto const &[id, name] : kNames) {
    EXPECT_EQ(config_param_name(id), name);

    const std::optional<ConfigParamID> found = config_param_id(name);

    if (found.has_value()) {
      EXPECT_EQ(*found, id) << name;
    }
    else {
      ADD_FAILURE() << name << " is not found by config_param_id";
    }
  }
}

// kHighestConfigParamID bounds every scan over the ids, so a parameter added
// past it stays invisible to config_param_id even once it is named.
TEST(ConfigParamNameTest, TheHighestIdIsTheLastOneNamed)
{
  EXPECT_EQ(std::to_underlying(kHighestConfigParamID),
            static_cast<std::int32_t>(kNames.size()) - 1);
  EXPECT_EQ(config_param_name(kHighestConfigParamID), kNames.back().second);
}

TEST(ConfigParamNameTest, AnUnnamedParameterIsNotFound)
{
  EXPECT_FALSE(config_param_id("").has_value());
  EXPECT_FALSE(config_param_id("max_rv32_memmory_bytes").has_value());
  // The ConfigParamID spelling is not the wire name.
  EXPECT_FALSE(config_param_id("kConfigParamMaxBytestringLen").has_value());
}

TEST(ConfigParamValueTest, ReadsTheArmTheParameterNames)
{
  std::uint32_t value = 1000;
  for (auto const &[id, name] : kNames) {
    ++value;

    const ConfigParam param = NetworkConfigParams::make_param(id, value);

    EXPECT_EQ(param.param(), id) << name;
    EXPECT_EQ(config_param_value(param), value) << name;
  }
}

// The gas budget is the one GasAmount arm: a value no uint32 arm could carry
// must survive the round trip rather than truncate.
TEST(ConfigParamValueTest, TheTxGasArmCarriesAFullGasAmount)
{
  constexpr std::uint64_t kValue = (std::uint64_t{1} << 40) + 7;
  EXPECT_EQ(config_param_max_value(ConfigParamID::kConfigParamMaxTxGas),
            std::numeric_limits<GasAmount>::max());
  EXPECT_EQ(config_param_max_value(
                ConfigParamID::kConfigParamMaxReturnBufferLen),
            std::numeric_limits<std::uint32_t>::max());

  const ConfigParam param =
      NetworkConfigParams::make_param(ConfigParamID::kConfigParamMaxTxGas,
                                      kValue);
  EXPECT_EQ(param.max_tx_gas(), kValue);
  EXPECT_EQ(config_param_value(param), kValue);

  NetworkConfig config;
  config.v(0);
  NetworkConfigParams::set_param(config, ConfigParamID::kConfigParamMaxTxGas,
                                 kValue);
  EXPECT_EQ(NetworkConfigParams(config.v0()).max_tx_gas, kValue);
}

// Narrowing would state a value the caller never asked for.
TEST(ConfigParamValueDeathTest, AUint32ArmRefusesAWiderValue)
{
  EXPECT_DEATH(NetworkConfigParams::make_param(
                   ConfigParamID::kConfigParamMaxCalldataLen,
                   std::uint64_t{std::numeric_limits<std::uint32_t>::max()} +
                       1),
               "");
}

// A configuration predating the parameter gets the default cap, not none.
TEST(NetworkConfigParamsTest, AListOmittingTxGasHasTheDefaultBudget)
{
  NetworkConfig config;
  config.v(0);
  NetworkConfigParams::set_param(config,
                                 ConfigParamID::kConfigParamMaxReturnBufferLen,
                                 kDefaultMaxReturnBufferLen);
  ASSERT_EQ(config.v0().params.size(),
            static_cast<std::size_t>(std::to_underlying(
                ConfigParamID::kConfigParamMaxReturnBufferLen)) +
                1);

  EXPECT_EQ(NetworkConfigParams(config.v0()).max_tx_gas, 1'000'000'000U);
}

// A configuration that states neither parameter admits every format the
// build reads, so it admits a newer one as soon as the build reads it.
TEST(NetworkConfigParamsTest, AListOmittingArtifactVersionsAdmitsEveryFormat)
{
  NetworkConfig config;
  config.v(0);
  NetworkConfigParams::set_param(config, ConfigParamID::kConfigParamMaxTxGas,
                                 kDefaultMaxTxGas);
  ASSERT_EQ(config.v0().params.size(),
            static_cast<std::size_t>(
                std::to_underlying(ConfigParamID::kConfigParamMaxTxGas)) +
                1);

  const NetworkConfigParams params(config.v0());
  EXPECT_EQ(params.min_rv32_artifact_version, 5U);
  EXPECT_EQ(params.max_rv32_artifact_version, 7U);
}

// Likewise for the syscall API: a configuration that states neither bound
// admits every minor the build implements.
TEST(NetworkConfigParamsTest, AListOmittingSyscallApiMinorsAdmitsEveryMinor)
{
  NetworkConfig config;
  config.v(0);
  NetworkConfigParams::set_param(
      config, ConfigParamID::kConfigParamMaxSubtransactionOutputBytes,
      kDefaultMaxSubtransactionOutputBytes);
  ASSERT_EQ(config.v0().params.size(),
            static_cast<std::size_t>(std::to_underlying(
                ConfigParamID::kConfigParamMinSyscallApiMinor)));

  const NetworkConfigParams params(config.v0());
  EXPECT_EQ(params.min_syscall_api_minor, 0U);
  EXPECT_EQ(params.max_syscall_api_minor,
            static_cast<std::uint32_t>(kSyscallApiMinor));
}

// Only minors the build implements may be stated.
TEST(NetworkConfigParamsTest, SyscallApiMinorsAreBoundedByTheBuild)
{
  for (const auto id : {ConfigParamID::kConfigParamMinSyscallApiMinor,
                        ConfigParamID::kConfigParamMaxSyscallApiMinor}) {
    EXPECT_EQ(config_param_min_value(id), 0U);
    EXPECT_EQ(config_param_max_value(id),
              static_cast<std::uint64_t>(kSyscallApiMinor));
  }
}

// The defaults are servable, and each way a range can leave what the build
// reads or implements is reported by naming the parameter and the bound.
TEST(NetworkConfigParamsTest, ReportsAdmissionRangesTheBuildCannotServe)
{
  EXPECT_EQ(unsupported_admission_range(NetworkConfigParams{}), std::nullopt);

  const auto reason = [](auto mutate) {
    NetworkConfigParams params;
    mutate(params);
    return unsupported_admission_range(params).value_or("");
  };
  const std::uint32_t newest_format = kMaxRv32ArtifactVersion;
  const std::uint32_t newest_minor = kSyscallApiMinor;

  EXPECT_EQ(reason([&](NetworkConfigParams &p) {
              p.max_rv32_artifact_version = newest_format + 1;
            }),
            "max_rv32_artifact_version 8 admits an RV32 artifact format newer "
            "than 7, the newest this build reads; run a build that reads "
            "format 8");
  EXPECT_EQ(reason([](NetworkConfigParams &p) {
              p.min_rv32_artifact_version = 4;
            }),
            "min_rv32_artifact_version 4 admits an RV32 artifact format older "
            "than 5, the oldest this build reads");
  EXPECT_EQ(reason([](NetworkConfigParams &p) {
              p.min_rv32_artifact_version = 7;
              p.max_rv32_artifact_version = 6;
            }),
            "min_rv32_artifact_version 7 exceeds max_rv32_artifact_version 6");
  EXPECT_EQ(reason([&](NetworkConfigParams &p) {
              p.max_syscall_api_minor = newest_minor + 1;
            }),
            "max_syscall_api_minor 3 admits syscall API 1.3, newer than the "
            "1.2 this build implements; run a build that implements it");
  EXPECT_EQ(reason([](NetworkConfigParams &p) {
              p.min_syscall_api_minor = 1;
              p.max_syscall_api_minor = 0;
            }),
            "min_syscall_api_minor 1 exceeds max_syscall_api_minor 0");
}

TEST(NetworkConfigParamsTest, StatedDefaultsStatesEveryParameterAtItsDefault)
{
  const NetworkConfig config = NetworkConfigParams::stated_defaults();

  EXPECT_TRUE(NetworkConfigParams::is_canonical(config.v0()));
  EXPECT_EQ(config.v0().params.size(), kNames.size());
  EXPECT_EQ(NetworkConfigParams(config.v0()), NetworkConfigParams{});
}

} // namespace hogd
