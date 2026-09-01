#include "hogd/hog_mempool/well_formed.h"

#include <algorithm>

#include "contract.hh"
#include "transaction.hh"

namespace hogd {

bool
MempoolAdmission::is_tx_well_formed(const SignedTransaction &tx)
{
  return std::ranges::all_of(tx.tx.contracts_to_deploy,
                             [](const Contract &contract) {
                               return contract.contract_type() !=
                                      ContractType::kNative;
                             });
}

} // namespace hogd
