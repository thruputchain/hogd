#include "hogd/hog_exec/runtime/item/native_contract_record.h"

#include <cstdint>
#include <memory>

#include "contract.hh"
#include "hogd/base/log.h"
#include "hogd/crypto/crypto.h"
#include "precompiles.hh"

namespace hogd {

std::shared_ptr<const Contract>
make_native_contract_record(HogexecPrecompiledNativeContracts contract_id)
{
  auto out = std::make_shared<Contract>();
  out->contract_type(ContractType::kNative);
  out->native_contract_id() = contract_id;
  return out;
}

DeployedContractHash
make_deployed_contract_hash(Contract const &contract)
{
  switch (contract.contract_type()) {
  case ContractType::kNative:
  case ContractType::kMockTest:
    return DeployedContractHash{.hash = sha256(contract),
                                .contract_size = 0,
                                .contract_type = contract.contract_type()};
  case ContractType::kWasmi:
    return DeployedContractHash{.hash = sha256(contract),
                                .contract_size = static_cast<uint32_t>(
                                    contract.wasmi_contract().data.size()),
                                .contract_type = ContractType::kWasmi};
  case ContractType::kRv32:
    return DeployedContractHash{.hash = sha256(contract),
                                .contract_size = static_cast<uint32_t>(
                                    contract.rv32_contract().data.size()),
                                .contract_type = ContractType::kRv32};
  }
  // ContractType is an XDR union discriminant, rejected at parse time if it is
  // not one of the arms above, so this is unreachable.
  panic("make_deployed_contract_hash: unsupported contract type: {}",
        contract.contract_type());
}

DeployedContractHash const &
default_wallet_deployed_contract_hash()
{
  static const DeployedContractHash hash =
      make_deployed_contract_hash(*make_native_contract_record(
          HogexecPrecompiledNativeContracts::kNativeContractDefaultWallet));
  return hash;
}

} // namespace hogd
