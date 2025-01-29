#pragma once

#include <cstring>
#include <type_traits>

#include "contract.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

constexpr static ContractAddress kDeployerAddress = ContractAddress{};

std::pair<SHA256Hash, std::shared_ptr<const Contract>>
load_built_rs_contract(std::string_view filename);

MethodID compute_sdk_name(std::string_view name);

// Optionally creates the contract if `contract_to_create` is not null.
SignedTransaction genesis_deployer_deploy_and_invoke(
    SHA256Hash const &nonce, SHA256Hash const &contract_hash,
    std::shared_ptr<const Contract> const &contract_to_create, MethodID method,
    Calldata const &calldata);

inline Calldata
make_calldata()
{
  return {};
}

template<typename C, typename... Cs>
Calldata static make_calldata(C const &data, Cs const &...additional)
{
  Calldata out;

  if constexpr (std::is_empty_v<C>) {
    // nothing to do
  }
  else if constexpr (std::is_trivially_copyable_v<C>) {
    const auto *addr = reinterpret_cast<const uint8_t *>(&data);
    out.resize_uninitialized(sizeof(C));
    std::memcpy(out.data(), addr, sizeof(C));
  }
  else if constexpr (x23::is_bytespan<C>) {
    out.resize_uninitialized(data.size());
    std::memcpy(out.data(), data.data(), data.size());
  }

  auto rest = make_calldata(additional...);

  out += rest;
  return out;
}

} // namespace hogd
