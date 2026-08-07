#pragma once

#include <memory>

#include "contract.hh"
#include "precompiles.hh"

namespace hogd {

// Builds the two-field Contract record for a native precompile.
std::shared_ptr<const Contract>
make_native_contract_record(HogexecPrecompiledNativeContracts contract_id);

// The on-ledger DeployedContractHash for a contract record (hash = sha256 of
// the serialized record; contract_size per type).
DeployedContractHash make_deployed_contract_hash(Contract const &contract);

// DeployedContractHash of the default_wallet precompile record — the target
// of root-resolution fallback (resolve_runtime) and the record genesis
// writes to the contract store.
DeployedContractHash const &default_wallet_deployed_contract_hash();

} // namespace hogd
