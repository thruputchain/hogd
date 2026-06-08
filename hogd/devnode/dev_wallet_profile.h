#pragma once

#include <memory>
#include <string_view>

#include "contract.hh"

namespace hogd {

inline constexpr std::string_view kDefaultDevWalletProfile =
    "hog-devnode-default";

struct DevWalletProfile {
  std::string_view name;
  std::string_view artifact_filename;
  std::string_view artifact_hint;
};

DevWalletProfile resolve_dev_wallet_profile(std::string_view name);

std::shared_ptr<Contract>
load_dev_wallet_contract(DevWalletProfile const &profile);

} // namespace hogd
