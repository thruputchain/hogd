#include "hogd/devnode/dev_wallet_profile.h"

#include <format>
#include <stdexcept>
#include <string_view>

#include "built_contracts/contract_loader.h"
#include "built_contracts/dev_wallet.h"

namespace hogd {

namespace {

constexpr DevWalletProfile kDefaultDevWallet{
    .name = kDefaultDevWalletProfile,
    .artifact_filename = dev_wallet::DevWallet::kWasmFile,
    .artifact_hint = "contracts/dev_wallet.wasm",
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
  return load_contract(profile.artifact_filename);
}

} // namespace hogd
