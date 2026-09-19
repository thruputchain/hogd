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

// The all-zero address, GenesisContractAddresses::kAddressNull. Reserved as
// the address where nothing lives, so "no address" conventions (a cleared
// approval, an unset auth token consumer, a mint or burn reported as a
// transfer) stay sound. Every other deployment address is a SHA-256 image, so
// genesis is the only place a contract could be put here by choice; genesis
// load refuses it.
inline constexpr ContractAddress kNullContractAddress =
    genesis_contract_address(GenesisContractAddresses::kAddressNull);
static_assert(kNullContractAddress == ContractAddress{},
              "the null address is the all-zero ContractAddress");

inline constexpr ContractAddress kNativeTokenContractAddress =
    genesis_contract_address(GenesisContractAddresses::kAddressNativeToken);

inline constexpr ContractAddress kGenesisRootContractAddress =
    genesis_contract_address(GenesisContractAddresses::kAddressGenesisRoot);

// Genesis deploys the native token and the root contract at fixed addresses;
// neither may be the reserved null address.
static_assert(kNativeTokenContractAddress != kNullContractAddress);
static_assert(kGenesisRootContractAddress != kNullContractAddress);

// Contract-data key reserved to the native token implementation. Ordinary
// contracts cannot write storage owned by the native token address.
inline constexpr StorageKeyContractData kNativeTokenTotalSupplyKey{};

} // namespace hogd
