#include "hogd/hog_mempool/well_formed.h"

#include "contract.hh"
#include "transaction.hh"

namespace hogd {

bool
is_tx_well_formed(SignedTransaction const &tx)
{
  for (Contract const &contract : tx.tx.contracts_to_deploy) {
    if (contract.contract_type() == ContractType::kNative) {
      return false;
    }
  }
  return true;
}

} // namespace hogd
