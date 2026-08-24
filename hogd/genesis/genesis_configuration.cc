#include "hogd/genesis/genesis_configuration.h"
#include "hogd/genesis/genesis_constants.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#include "address_auth.hh"
#include "built_contracts/contract_loader.h"
#include "built_contracts/genesis_root.h"
#include "built_contracts/genesis_root.hh"
#include "contract.hh"
#include "genesis.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_exec/runtime/contract_trait_client.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "precompiles.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

namespace {
std::tuple<std::uint64_t, std::uint64_t>
make_contract_u128(uint128_t value)
{
  return {static_cast<std::uint64_t>(value >> 64),
          static_cast<std::uint64_t>(value)};
}

std::shared_ptr<const Contract>
make_native_contract(HogexecPrecompiledNativeContracts contract_id)
{
  auto out = std::make_shared<Contract>();
  out->contract_type(ContractType::kNative);
  out->native_contract_id() = contract_id;
  return out;
}

DeployedContractHash
make_deployed_contract_hash(std::shared_ptr<const Contract> const &contract)
{
  assert(contract);
  switch (contract->contract_type()) {
  case ContractType::kNative:
  case ContractType::kMockTest:
    return DeployedContractHash{.hash = sha256(*contract),
                                .contract_size = 0,
                                .contract_type = contract->contract_type()};
  case ContractType::kWasmi:
    return DeployedContractHash{.hash = sha256(*contract),
                                .contract_size = static_cast<uint32_t>(
                                    contract->wasmi_contract().data.size()),
                                .contract_type = ContractType::kWasmi};
  case ContractType::kRv32:
    return DeployedContractHash{.hash = sha256(*contract),
                                .contract_size = static_cast<uint32_t>(
                                    contract->rv32_contract().data.size()),
                                .contract_type = ContractType::kRv32};
  }
  // ContractType is an XDR union discriminant, rejected at parse time if it is
  // not one of the arms above, so this is unreachable.
  panic("make_deployed_contract_hash: unsupported contract type: {}",
        contract->contract_type());
}
} // namespace

DeployedContractHash
native_default_wallet_hash()
{
  return make_deployed_contract_hash(make_native_contract(
      HogexecPrecompiledNativeContracts::kNativeContractDefaultWallet));
}

bool
valid_genesis_configuration(GenesisConfiguration const &config)
{
  return config.root_contract.contract_type() == ContractType::kRv32 &&
         !config.root_contract.rv32_contract().data.empty();
}

void
visit_genesis_execution_prerequisites(
    GenesisConfiguration const &config,
    std::function<void(std::shared_ptr<const Contract>)> const &visit_contract,
    std::function<void(ContractAddress const &,
                       DeployedContractHash const &)> const &visit_deployment,
    std::function<void(ContractAddress const &, AddressAuthData const &)> const
        &visit_auth_data)
{
  CHECK(valid_genesis_configuration(config));

  auto root = std::make_shared<Contract>(config.root_contract);
  visit_contract(root);
  visit_deployment(kGenesisRootContractAddress,
                   make_deployed_contract_hash(root));

  AddressAuthData root_auth{};
  root_auth.required_weight = 0;
  root_auth.last_written_height = 0;
  visit_auth_data(kGenesisRootContractAddress, root_auth);

  auto token = make_native_contract(
      HogexecPrecompiledNativeContracts::kNativeContractHogToken);
  visit_contract(token);
  visit_deployment(kNativeTokenContractAddress,
                   make_deployed_contract_hash(token));

  auto wallet = make_native_contract(
      HogexecPrecompiledNativeContracts::kNativeContractDefaultWallet);
  visit_contract(wallet);
}

std::shared_ptr<const Contract>
load_stock_genesis_root_contract()
{
  auto contract = std::make_shared<Contract>(ContractType::kRv32);
  contract->rv32_contract() = load_contract_blob("genesis_root.rv32a");
  return contract;
}

GenesisConfiguration
GenesisConfigurationBuilder::build(NetworkHash const &network_hash,
                                   StorageCostModel const &cost_model,
                                   Contract const &root_contract) const
{
  CHECK_EQ(root_contract.contract_type(), ContractType::kRv32);
  CHECK(!root_contract.rv32_contract().data.empty());
  return GenesisConfiguration{
      .network_hash = network_hash,
      .storage_cost_model = cost_model.to_xdr(),
      .root_contract = root_contract,
      .default_wallet =
          default_wallet_override_.value_or(native_default_wallet_hash()),
  };
}

std::expected<std::vector<SignedTransaction>, GenesisCeremonyError>
GenesisConfigurationBuilder::try_make_genesis_ceremony_transactions(
    StorageCostModel const &cost_model) const
{
  using root_client_t = ContractTraitClient<genesis_root::GenesisRoot>;
  root_client_t client(kGenesisRootContractAddress);
  constexpr static GasAmount kDistributionGas =
      std::numeric_limits<GasAmount>::max();
  // RV32 contracts have a 64 KiB address space. Keep the decoded distribution
  // vector comfortably below that limit, including allocator and contract
  // stack space. A ceremony is one atomic height-1 block, but it may contain
  // several root transactions.
  constexpr static std::size_t kDistributionsPerTransaction = 16;

  // Every transaction executes independently against the height-0 snapshot,
  // so make each one self-funding. The minted funding is consumed by the rows
  // that transaction creates; no economic state needs to be prewritten.
  const uint128_t root_funding =
      static_cast<uint128_t>(cost_model.get_balance_cost()) +
      static_cast<uint128_t>(cost_model.get_replay_cache_cost(1));
  const uint128_t token_funding =
      static_cast<uint128_t>(cost_model.get_balance_cost()) +
      static_cast<uint128_t>(cost_model.get_nnint_cost());

  // The root contract mints each distribution plus enough native token to
  // create the root replay/balance rows and token supply/balance rows for every
  // ceremony transaction. Reject the artifact before emitting transactions if
  // all of those mints would exceed the protocol supply.
  uint128_t remaining_supply =
      uint128_t{kTotalHogSupply} * uint128_t{kHogDivisibility};
  const std::size_t transaction_count =
      std::max<std::size_t>(1, (entries_.size() + kDistributionsPerTransaction -
                                1) /
                                   kDistributionsPerTransaction);
  for (std::size_t i = 0; i < transaction_count; ++i) {
    if (root_funding > remaining_supply) {
      return std::unexpected(GenesisCeremonyError::kSupplyExceeded);
    }
    remaining_supply -= root_funding;
    if (token_funding > remaining_supply) {
      return std::unexpected(GenesisCeremonyError::kSupplyExceeded);
    }
    remaining_supply -= token_funding;
  }
  for (Entry const &entry : entries_) {
    if (entry.amount < 0) {
      return std::unexpected(GenesisCeremonyError::kNegativeDistribution);
    }
    const auto amount = static_cast<uint128_t>(entry.amount);
    if (amount > remaining_supply) {
      return std::unexpected(GenesisCeremonyError::kSupplyExceeded);
    }
    remaining_supply -= amount;
  }

  std::vector<SignedTransaction> transactions;
  transactions.reserve(transaction_count);

  for (std::size_t begin = 0; begin < std::max<std::size_t>(1, entries_.size());
       begin += kDistributionsPerTransaction) {
    const std::size_t end =
        std::min(entries_.size(), begin + kDistributionsPerTransaction);
    x23::Vec<genesis_root::Distribution, UINT32_MAX> distributions;
    decltype(Transaction{}.contracts_to_deploy) contracts_to_deploy;
    std::unordered_set<SHA256Hash, SipHash24Hasher> attached_contract_hashes;
    distributions.reserve(end - begin);
    attached_contract_hashes.reserve(end - begin);
    for (std::size_t i = begin; i < end; ++i) {
      auto const &entry = entries_[i];
      distributions.push_back(genesis_root::Distribution{
          .nonce = entry.initial_nonce,
          .contract_hash = entry.contract_hash,
          .initializer = entry.entrypoint,
          .initializer_calldata = entry.calldata,
          .allocation = to_xdr(static_cast<uint128_t>(entry.amount)),
      });
      // A transaction attaches contract code by hash. Attach each distinct
      // artifact only once within the transaction, even when many accounts use
      // the same wallet implementation.
      if (attached_contract_hashes.insert(entry.contract_hash).second) {
        contracts_to_deploy.push_back(*entry.deployed_contract);
      }
    }

    transactions.push_back(SignedTransaction{
        .tx = Transaction{
            .invocation = client.make_invocation<u8"distribute">(
                kDistributionGas, distributions,
                make_contract_u128(root_funding),
                make_contract_u128(token_funding)),
            .uniquifier =
                {
                    .block_expiry_height =
                        std::numeric_limits<BlockHeight>::max(),
                    .nonce = transactions.size(),
                },
            .contracts_to_deploy = std::move(contracts_to_deploy),
        }});
  }
  return transactions;
}

std::vector<SignedTransaction>
GenesisConfigurationBuilder::make_genesis_ceremony_transactions(
    StorageCostModel const &cost_model) const
{
  auto transactions = try_make_genesis_ceremony_transactions(cost_model);
  CHECK(transactions.has_value());
  return std::move(*transactions);
}

ContractAddress
calculate_genesis_deployment_address(SHA256Hash const &contract_hash,
                                     SHA256Hash const &nonce)
{
  ContractDeploymentHashPreimage preimage{
      .deployer_address = kGenesisRootContractAddress,
      .nonce = nonce,
      .contract_hash = contract_hash,
  };

  return sha256(preimage);
}

void
GenesisConfigurationBuilder::add_distribution(
    std::shared_ptr<const Contract> const &contract, SHA256Hash const &nonce,
    ContractEntrypointID entrypoint, Calldata calldata, int128_t amount)
{
  entries_.push_back({
      .initial_nonce = nonce,
      .contract_hash = sha256(*contract),
      .entrypoint = entrypoint,
      .calldata = std::move(calldata),
      .amount = amount,
      .deployed_contract = contract,
  });
}
} // namespace hogd
