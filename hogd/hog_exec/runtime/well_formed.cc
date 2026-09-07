#include "hogd/hog_exec/runtime/well_formed.h"

#include <algorithm>

#include "contract.hh"
#include "transaction.hh"

namespace hogd {

bool
HogexecAdmission::is_contract_attachable(const Contract &contract)
{
  return contract.contract_type() != ContractType::kNative;
}

bool
HogexecAdmission::is_tx_well_formed(const Transaction &tx)
{
  return std::ranges::all_of(tx.contracts_to_deploy, is_contract_attachable);
}

} // namespace hogd
