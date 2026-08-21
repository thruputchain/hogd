#pragma once

#include "genesis.hh"
#include "storage_item.hh"

namespace hogd {

constexpr ContractAddress
genesis_contract_address(GenesisContractAddresses value)
{
  ContractAddress address{};
  const uint32_t raw = std::to_underlying(value);
  address[28] = static_cast<uint8_t>(raw >> 24);
  address[29] = static_cast<uint8_t>(raw >> 16);
  address[30] = static_cast<uint8_t>(raw >> 8);
  address[31] = static_cast<uint8_t>(raw);
  return address;
}

inline constexpr ContractAddress kNativeTokenContractAddress =
    genesis_contract_address(GenesisContractAddresses::kAddressNativeToken);

inline constexpr ContractAddress kGenesisRootContractAddress =
    genesis_contract_address(GenesisContractAddresses::kAddressGenesisRoot);

// Contract-data key reserved to the native token implementation. Ordinary
// contracts cannot write storage owned by the native token address.
inline constexpr StorageKeyContractData kNativeTokenTotalSupplyKey{};

} // namespace hogd
