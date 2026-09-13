#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

#include "hogd/base/network_config_params.h"
#include "network_config.hh"

namespace hogd {

// The names hog-devnode's --network-config flag, the overrides it records in
// a data directory's metadata, and the genesis JSON key parameters by.
// Spelled out rather than derived, so this pins the wire spelling: a rename
// that changes any of those three surfaces has to change this list too.
constexpr std::array<std::pair<ConfigParamID, std::string_view>, 10> kNames{{
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

} // namespace hogd
