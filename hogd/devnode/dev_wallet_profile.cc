#include "hogd/devnode/dev_wallet_profile.h"

#include <format>
#include <memory>
#include <stdexcept>
#include <string_view>

#include "built_contracts/contract_loader.h"
#include "contract.hh"

namespace hogd {

namespace {

constexpr DevWalletProfile kDefaultDevWallet{
    .name = kDefaultDevWalletProfile,
    .artifact_filename = "dev_wallet.rv32a",
    .artifact_hint = "contracts/dev_wallet.rv32a",
};

} // namespace

DevWalletProfile
resolve_dev_wallet_profile(std::string_view name)
{
  if (name == kDefaultDevWallet.name) {
    return kDefaultDevWallet;
  }
  throw std::runtime_error(
      std::format("unsupported dev wallet profile '{}', expected '{}'", name,
                  kDefaultDevWallet.name));
}

std::shared_ptr<Contract>
load_dev_wallet_contract(DevWalletProfile const &profile)
{
  auto contract = std::make_shared<Contract>(ContractType::kRv32);
  contract->rv32_contract() = load_contract_blob(profile.artifact_filename);
  return contract;
}

} // namespace hogd
