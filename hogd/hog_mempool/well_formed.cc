#include "hogd/hog_mempool/well_formed.h"

#include <algorithm>

#include "contract.hh"
#include "transaction.hh"

namespace hogd {

bool
is_tx_well_formed(SignedTransaction const &tx)
{
  return std::ranges::all_of(tx.tx.contracts_to_deploy,
                             [](Contract const &contract) {
                               return contract.contract_type() !=
                                      ContractType::kNative;
                             });
}

} // namespace hogd
