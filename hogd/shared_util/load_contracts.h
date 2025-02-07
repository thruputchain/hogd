#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>

#include "contract.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"

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
    // BCS representation for lengths < 128 is one byte, so we'll
    // do a quick hack that works for those lengths
    if (data.size() > 127)
      throw std::runtime_error("span arg too big for BCS hack");

    const size_t HDRLEN = 1;
    out.resize_uninitialized(HDRLEN + data.size());

    const uint8_t bcs_len = static_cast<uint8_t>(data.size());
    std::memcpy(out.data(), &bcs_len, HDRLEN);

    std::memcpy(HDRLEN + out.data(), data.data(), data.size());
  }

  auto rest = make_calldata(additional...);

  out += rest;
  return out;
}

} // namespace hogd
